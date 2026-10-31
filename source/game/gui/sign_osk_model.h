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

#ifndef SIGN_OSK_MODEL_H
#define SIGN_OSK_MODEL_H

#include <stddef.h>
#include <stdint.h>

#define SIGN_OSK_COLS 16
#define SIGN_OSK_LINES 4
#define SIGN_OSK_SIZE (SIGN_OSK_COLS * SIGN_OSK_LINES)

/* Beta's own per-line ceiling -- narrower than the 16-wide grid above.
 * [740c583 GuiEditSign.keyTyped]: a new character is accepted only when
 * `allowedCharacters.indexOf(var1) >= 0 && signText[editLine].length() < 15`
 * -- character-count only. Verified against the decompile: GuiEditSign never
 * calls FontRenderer.getStringWidth, so there is no pixel-width term (the
 * maintainer ruling 2026-08-17 asked this to be checked, not assumed).
 * SIGN_OSK_COLS stays 16 for CaveWii's own per-cell window geometry
 * (documented deviation, aca219c); the 16th column (index
 * SIGN_BETA_MAX_CHARS) may never hold a printed character. */
#define SIGN_BETA_MAX_CHARS 15

struct sign_osk_edit {
	uint8_t slot;
	uint8_t ch;
};

size_t sign_osk_shadow_from_cells(const uint8_t cells[SIGN_OSK_SIZE],
								  size_t line,
								  char shadow[SIGN_OSK_COLS + 1]);
size_t sign_osk_diff_cells(const uint8_t cells[SIGN_OSK_SIZE], size_t line,
						   const char* shadow,
						   struct sign_osk_edit edits[SIGN_OSK_COLS]);

#endif
