/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

#include "sign_osk_model.h"

#include <assert.h>

size_t sign_osk_shadow_from_cells(const uint8_t cells[SIGN_OSK_SIZE],
								  size_t line,
								  char shadow[SIGN_OSK_COLS + 1]) {
	assert(cells && line < SIGN_OSK_LINES && shadow);
	size_t base = line * SIGN_OSK_COLS;
	for(size_t col = 0; col < SIGN_OSK_COLS; col++)
		shadow[col] = (char)cells[base + col];

	/* CaveWii's fixed cell window is adapted to Beta's append/backspace string
	 * model by dropping only trailing spaces. Leading/interior spaces stay in
	 * the edit string. [740c583 GuiEditSign.keyTyped] */
	size_t len = SIGN_OSK_COLS;
	while(len > 0 && shadow[len - 1] == ' ')
		len--;
	shadow[len] = 0;
	return len;
}

size_t sign_osk_diff_cells(const uint8_t cells[SIGN_OSK_SIZE], size_t line,
						   const char* shadow,
						   struct sign_osk_edit edits[SIGN_OSK_COLS]) {
	assert(cells && line < SIGN_OSK_LINES && shadow && edits);
	size_t len = 0;
	while(len < SIGN_OSK_COLS && shadow[len] != 0)
		len++;

	size_t count = 0;
	size_t base = line * SIGN_OSK_COLS;
	for(size_t col = 0; col < SIGN_OSK_COLS; col++) {
		uint8_t target = col < len ? (uint8_t)shadow[col] : ' ';
		if(cells[base + col] != target)
			edits[count++] = (struct sign_osk_edit) {
				.slot = (uint8_t)(base + col),
				.ch = target,
			};
	}
	return count;
}
