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

#include "chest_link.h"

const int CHEST_LINK_DX[4] = {-1, 1, 0, 0};
const int CHEST_LINK_DZ[4] = {0, 0, -1, 1};

int chest_neighbor_count(const bool neighbor_is_chest[4]) {
	int n = 0;
	for(int i = 0; i < 4; i++)
		if(neighbor_is_chest[i])
			n++;
	return n;
}

bool chest_can_place(const bool neighbor_is_chest[4],
					 const bool neighbor_already_paired[4]) {
	if(chest_neighbor_count(neighbor_is_chest) > 1)
		return false;

	for(int i = 0; i < 4; i++)
		if(neighbor_is_chest[i] && neighbor_already_paired[i])
			return false;

	return true;
}

int chest_partner_index(const bool neighbor_is_chest[4]) {
	for(int i = 0; i < 4; i++)
		if(neighbor_is_chest[i])
			return i;

	return -1;
}

bool chest_is_primary(int ax, int az, int bx, int bz) {
	if(ax != bx)
		return ax < bx;

	return az < bz;
}
