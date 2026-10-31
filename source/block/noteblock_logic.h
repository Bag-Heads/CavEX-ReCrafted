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

#ifndef NOTEBLOCK_LOGIC_H
#define NOTEBLOCK_LOGIC_H

#include <stdint.h>

#include "blocks_data.h" // enum block_material

// Faithful Beta 1.7.3 TileEntityNote/BlockNote (decompile 740c583). The pitch
// cycle, instrument classification and pitch-formula math are isolated here as
// pure functions so they are headless golden-testable (tests/
// test_noteblock_jukebox_env.c), mirroring dispenser_logic.h/furnace_logic.h's
// split of pure math from the world/entity glue (server_noteblock.c /
// block_noteblock.c).

#define NOTEBLOCK_NOTE_MIN 0
#define NOTEBLOCK_NOTE_MAX 24
#define NOTEBLOCK_NOTE_COUNT 25 // NOTEBLOCK_NOTE_MAX + 1

// TileEntityNote.changePitch's value half: the note advances by one, modulo
// 25. Cycles 0..24 then wraps to 0. [decompile 740c583
// TileEntityNote.changePitch]
uint8_t noteblock_change_pitch(uint8_t note);

// TileEntityNote.triggerNote's instrument switch, keyed off the material of
// the block BELOW the note block (Material.rock/sand/glass/wood -> 1/2/3/4,
// anything else -> 0/harp). The four checks are independent `if`s in the
// decompile (no `else`), transcribed here as an exclusive switch since
// block_material is a discrete enum -- behaviourally identical, easier to
// read. [decompile 740c583 TileEntityNote.triggerNote]
enum note_instrument {
	NOTE_INSTRUMENT_HARP = 0,	   // "harp" (also the default/else case)
	NOTE_INSTRUMENT_BD = 1,		   // "bd" (bass drum), Material.rock
	NOTE_INSTRUMENT_SNARE = 2,	   // "snare", Material.sand
	NOTE_INSTRUMENT_HAT = 3,	   // "hat", Material.glass
	NOTE_INSTRUMENT_BASSATTACK = 4 // "bassattack", Material.wood
};
enum note_instrument noteblock_instrument_for_material(enum block_material mat);

// BlockNote.playBlock's pitch formula, Beta's EXACT double-precision Math.pow
// then narrowed to float (matches the decompile's cast chain, not a
// float-only reimplementation): (float)Math.pow(2.0D, (double)(note-12) /
// 12.0D). Spot values: note 0 -> 0.5F, 12 -> 1.0F, 24 -> 2.0F exactly (all
// clean powers of two -> exact fp32 representation, no tolerance needed).
// [decompile 740c583 BlockNote.playBlock]
float noteblock_pitch(uint8_t note);

#endif
