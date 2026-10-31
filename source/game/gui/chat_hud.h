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

#ifndef CHAT_HUD_H
#define CHAT_HUD_H

#include <stdbool.h>

struct chat_ring_state;

/* Beta's chat block [740c583 GuiIngame.renderGameOverlay]: lines stack
 * UPWARD from `base_y` (the newest line's text top, in physical px; Beta
 * uses scaledHeight - 48), 9 logical px apart, each with a black rect from
 * x = 2 to 322 logical px at alpha/2 and white shadowed text at alpha.
 * Closed HUD: at most 10 lines, a line is hidden once its updateCounter
 * reaches 200 and fades quadratically over the last 20 ticks. Open (the
 * composer is up): every line opaque, at most `max_lines` (Beta 20; the
 * composer passes what fits above its keyboard). `x` is the physical left
 * edge of the block (Beta 2 logical px). */
void chat_hud_render(const struct chat_ring_state* ring, int x, int base_y,
					 bool open, int max_lines);

/* The OPEN composer's history band (docs/ui_design/chat-keyboard/SPEC.md,
 * "World remains live"): every line opaque, at most `max_lines`, and both the
 * strip AND the text clipped to `width` physical px instead of Beta's fixed
 * 320-logical strip -- the band is 552 px wide and sits beside the panel, so a
 * 640-px strip would run outside it. The stored ring line is never rewrapped:
 * an overlong line simply stops at the right edge. The closed HUD keeps
 * chat_hud_render and is unchanged. */
void chat_hud_render_open(const struct chat_ring_state* ring, int x, int base_y,
						  int max_lines, int width);

/* Number of lines chat_hud_render would draw right now (for layout). */
int chat_hud_visible_lines(const struct chat_ring_state* ring, bool open,
						   int max_lines);

/* ---- the rendered block's BOTTOM, and the H-C2 ceiling (build spec 15 A.3)
 * The closed HUD's black strip is Beta's fixed 320-LOGICAL one, i.e. the full
 * 640 px viewport width at GUI scale 2, so the history block reaches the
 * bottom-right corner the chat socket occupies and needs a VERTICAL ceiling.
 *
 * chat_hud_bottom is the lowest pixel the closed block reaches for a given
 * `base_y` (the newest line's text top): the newest line's 8*scale-tall text
 * plus gutil_text's hard shadow, whose offset is `scale / 8` and therefore
 * GFX_GUI_SCALE at the 8*GFX_GUI_SCALE the closed HUD draws at (review 28 L1
 * -- it is not a fixed 2 px, it only evaluates to 2 at the shipped scale).
 * Lines stack UPWARD, so the newest line alone fixes the bottom, and the
 * per-line strip -- one GUI unit higher and 9 units tall -- ends one shadow
 * offset above it, which is why the SHADOW is what the bounds actually end
 * on.
 *
 * chat_hud_clamp_base_y lifts `base_y` by exactly the amount chat_hud_bottom
 * would pass `bottom_limit`, and returns an anchor that already clears the
 * limit UNCHANGED ("keep any existing higher position"). It is a pure
 * function on the CALLER's side on purpose: neither renderer below changes,
 * so the composer's open band and single player are untouched. */
int chat_hud_bottom(int base_y);
int chat_hud_clamp_base_y(int base_y, int bottom_limit);

/* Logical-px width of a display line at the 8-px font (FontRenderer.
 * getStringWidth: \247 colour pairs are zero-width). The wrap-at-push
 * callback for chat_ring_push_wrapped. */
int chat_hud_line_width(const char* text);

#endif
