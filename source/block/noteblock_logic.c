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

#include "noteblock_logic.h"

#include <math.h>

uint8_t noteblock_change_pitch(uint8_t note) {
	return (uint8_t)((note + 1) % NOTEBLOCK_NOTE_COUNT);
}

enum note_instrument
noteblock_instrument_for_material(enum block_material mat) {
	switch(mat) {
		case MATERIAL_STONE: return NOTE_INSTRUMENT_BD;
		case MATERIAL_SAND: return NOTE_INSTRUMENT_SNARE;
		case MATERIAL_GLASS: return NOTE_INSTRUMENT_HAT;
		case MATERIAL_WOOD: return NOTE_INSTRUMENT_BASSATTACK;
		default: return NOTE_INSTRUMENT_HARP;
	}
}

float noteblock_pitch(uint8_t note) {
	// Beta computes this in DOUBLE precision then narrows to float on return
	// (the method's return type is float, not the expression itself) --
	// replicate the exact cast chain rather than doing float-only math, per
	// CODESTYLE's float-cast-chain discipline.
	return (float)pow(2.0, (double)((int)note - 12) / 12.0);
}
