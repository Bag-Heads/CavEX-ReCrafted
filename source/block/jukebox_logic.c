/*
	This file is part of CaveWii_v2 (a CavEX/fCavEX derivative).

	CaveWii_v2 is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CaveWii_v2 is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CaveWii_v2.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "jukebox_logic.h"

void jukebox_eject_offset(float r0, float r1, float r2, float* dx, float* dy,
						  float* dz) {
	const float f = 0.7F;
	*dx = r0 * f + (1.0F - f) * 0.5F;
	*dy = r1 * f + (1.0F - f) * 0.2F + 0.6F;
	*dz = r2 * f + (1.0F - f) * 0.5F;
}
