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

#include <math.h>
#include <stdbool.h>

#include "beta_math.h"
#include "jmath.h"

#ifndef BETA_MATH_PI
#define BETA_MATH_PI 3.141592653589793
#endif

// Beta MathHelper.SIN_TABLE: SIN_TABLE[i] = (float)Math.sin(i * PI * 2 / 65536)
// with the argument computed in DOUBLE precision (matching vanilla), result
// narrowed to float.
static float sin_table[65536];
static bool table_ready = false;

void beta_math_init(void) {
	if(table_ready)
		return;
	for(int i = 0; i < 65536; i++)
		sin_table[i] = (float)sin((double)i * BETA_MATH_PI * 2.0 / 65536.0);
	table_ready = true;
}

float mh_sin(float x) {
	if(!table_ready)
		beta_math_init();
	return sin_table[java_float_to_int(x * 10430.378f) & 0xFFFF];
}

float mh_cos(float x) {
	if(!table_ready)
		beta_math_init();
	return sin_table[java_float_to_int(x * 10430.378f + 16384.0f) & 0xFFFF];
}
