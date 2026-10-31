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

#ifndef PISTON_MOBILITY_H
#define PISTON_MOBILITY_H

// Beta 1.7.3 Block.getMobilityFlag() -> Material.getMaterialMobility() (0=movable, 1=destroyed-on-push,
// 2=immovable), with the BlockBed/BlockDoor/BlockPressurePlate overrides folded in. A pure id->flag
// query over the CaveWii block-id space, used by the piston push logic (redstone_piston.c canPushBlock /
// canExtend / tryExtend) through the injected get_mobility_flag accessor. Decompile-cited per entry in
// the .c. The table is the SPEC: a golden re-pins every non-zero entry (tests/test_piston_mobility_golden).
int piston_mobility_flag(int id);

#endif
