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

#ifndef BETA_TEXT_COLOR_H
#define BETA_TEXT_COLOR_H

/*
	Beta 1.7.3 colour codes, shared by every text path that can show server
	text (gui_util.c's gutil_text_rgba/gutil_font_width for chat, disconnect
	reasons and the server list; render_sign_text.c for in-world signs).
	Header-only and GL/GX-free so pure geometry TUs can include it.

	Strings arrive in this engine's single-byte display charset: U+00A7 is
	the raw byte 0xA7 ('\247') -- see mp_codec.c mp_utf16_to_display and the
	Packet130 sign path in server_remote_protocol.c (units <= 0xFF copied
	verbatim).

	[mcb173-retromcp FontRenderer.java:83-111] builds 16 normal and 16 shadow
	colours. The low three index bits select 170 per RGB channel; bit three
	adds 85 to every channel. Entry 6 adds another 85 to red. Each shadow
	channel is divided by four after that adjustment.
	[FontRenderer.java:147-161] renderString consumes a section sign and its
	following selector together. Hexadecimal selectors are case-insensitive;
	unknown selectors choose white. The shadow pass selects the corresponding
	shadow entry. Thus k/l/m/n/o/r (1.4+ formatting) turn the text white here,
	and neither char is drawn. A
	trailing lone 167 is left to the glyph lookup, where it is not in
	font.txt's allowed set, so it draws nothing.
	[FontRenderer.java:183-202] getStringWidth: a 167 skips itself and the
	following char (`++var3`), so codes contribute zero width.
*/

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BETA_TEXT_SECTION ((char)0xA7)

/* Selector char -> colour index, Beta's indexOf-on-lowercase with the
 * out-of-range fallback to 15 (white). */
static inline int beta_text_code_index(char c) {
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return 15;
}

/* Colour index 0..15 -> RGB; `shadow` selects Beta's darker list entry
 * index + 16. Built from Beta's own loop variable [FontRenderer.java:83-106]:
 * the gold boost `if(var9 == 6)` tests the LIST index, so it applies to
 * entry 6 only, never to its shadow entry 22 -- gold's shadow is
 * (170,170,0)/4 = (42,42,0), not (255,170,0)/4. */
static inline void beta_text_code_rgb(int index, bool shadow, uint8_t rgb[3]) {
	int list = (index & 15) + (shadow ? 16 : 0); /* Beta's var9, 0..31 */
	int base = (list >> 3 & 1) * 85;
	int r = (list >> 2 & 1) * 170 + base;
	int g = (list >> 1 & 1) * 170 + base;
	int b = (list >> 0 & 1) * 170 + base;
	if(list == 6)
		r += 85;
	if(list >= 16) {
		r /= 4;
		g /= 4;
		b /= 4;
	}
	rgb[0] = (uint8_t)r;
	rgb[1] = (uint8_t)g;
	rgb[2] = (uint8_t)b;
}

/* Beta's shadow tint of an explicit base colour:
 * [FontRenderer.java:127-131] `(var4 & 16579836) >> 2` == per-channel
 * (c & 0xFC) >> 2 == c / 4 (floor). */
static inline uint8_t beta_text_shadow_channel(uint8_t c) {
	return (uint8_t)((c & 0xFC) >> 2);
}

/* True when str[i] starts a colour code (a 0xA7 with a following char
 * inside `len`). The caller consumes 2 chars. */
static inline bool beta_text_is_code(const char* str, size_t i, size_t len) {
	return str[i] == BETA_TEXT_SECTION && i + 1 < len;
}

/* Beta getStringWidth over the first `len` bytes, in 8-unit font space:
 * glyph_width[ch] + 1 per drawn char (this engine's split charWidth). */
static inline int beta_text_width(const char* str, size_t len,
								  const uint8_t glyph_width[256]) {
	int width = 0;
	for(size_t i = 0; i < len; i++) {
		if(str[i] == BETA_TEXT_SECTION)
			i++; /* skips the selector; a lone trailing 167 adds nothing */
		else
			width += (int)glyph_width[(unsigned char)str[i]] + 1;
	}
	return width;
}

#endif
