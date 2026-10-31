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

#ifndef JUKEBOX_LOGIC_H
#define JUKEBOX_LOGIC_H

// Faithful Beta 1.7.3 BlockJukeBox/TileEntityRecordPlayer (decompile
// 740c583). The eject-offset math is isolated here as a pure function (three
// ALREADY-DRAWN floats in, three offsets out) so the RNG DRAW ORDER at the
// call site (server_jukebox.c: x, then y, then z, on the world rand) is
// independently golden-testable without a live world, mirroring
// dispenser_logic.c's split of pure math from entity/world glue.

// Beta EntityItem.delayBeforeCanPickup, set explicitly by
// BlockJukeBox.func_28038_b_ on the ejected record (10 ticks). FH-12
// (2026-08-22): entity_item.c now has a real per-instance pickup_delay field
// (entity.h's struct entity_item) -- this constant IS wired to it at the
// eject call site (server_local_jukebox_eject in server_local.c). It happens
// to equal the SAME 10-tick value that the generic
// Block.dropBlockAsItem_do/Entity.entityDropItem paths use (ordinary block
// breaks, mob drops, dispenser-generic-item eject is the one exception --
// BlockDispenser.dispense never touches the field, so it stays 0), but this
// name/constant is kept distinct because BlockJukeBox's citation is its own
// call site, independent of that shared default.
#define JUKEBOX_EJECT_PICKUP_DELAY 10

// BlockJukeBox.func_28038_b_'s ejected-item spawn offset, VERBATIM:
//   f = 0.7F
//   dx = r0*f + (1-f)*0.5   (== r0*0.7 + 0.15)
//   dy = r1*f + (1-f)*0.2 + 0.6   (== r1*0.7 + 0.66)
//   dz = r2*f + (1-f)*0.5   (== r2*0.7 + 0.15)
// r0/r1/r2 are three SEPARATE nextFloat() draws, in x/y/z order -- the
// caller supplies them already drawn so the draw order stays visible at the
// call site (the dispenser generic-item-eject precedent).
// [decompile 740c583 BlockJukeBox.func_28038_b_]
void jukebox_eject_offset(float r0, float r1, float r2, float* dx, float* dy,
						  float* dz);

#endif
