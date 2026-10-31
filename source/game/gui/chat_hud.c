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

#include "chat_hud.h"

#include <string.h>

#include "../../platform/gfx.h"
#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../game_state.h"

/* [740c583 GuiIngame.renderGameOverlay chat block]:
 *   d = 1 - updateCounter / 200; d *= 10; clamp 0..1; d *= d;
 *   alpha = (int)(255 * d) (255 while GuiChat is open)
 * Returns 0 for a line that must not be drawn. */
static int chat_line_alpha(int age, bool open) {
	if(open)
		return 255;
	if(age >= 200)
		return 0;
	double d = 1.0 - (double)age / 200.0;
	d *= 10.0;
	if(d < 0.0)
		d = 0.0;
	if(d > 1.0)
		d = 1.0;
	d *= d;
	return (int)(255.0 * d);
}

int chat_hud_visible_lines(const struct chat_ring_state* ring, bool open,
						   int max_lines) {
	if(!ring || max_lines <= 0)
		return 0;
	int shown = 0;
	/* i = 0 is the NEWEST line (Beta's list index 0). */
	for(size_t i = 0; i < ring->count && (int)i < max_lines; i++) {
		size_t idx = (ring->head + ring->count - 1 - i) % CHAT_RING_CAPACITY;
		if(chat_line_alpha(ring->age[idx], open) > 0)
			shown++;
	}
	return shown;
}

/* `strip_w` is the black strip's width in physical px; `clip_w` > 0 also clips
 * the TEXT to that width (the open composer's 552-px history band -- SPEC
 * "Long open lines clip at the right, not rewrap the stored ring"). The closed
 * HUD passes the Beta 320-logical strip and no text clip, so its output is
 * unchanged byte for byte. */
/* chat_clip_display takes a width function of one argument; the open band's
 * font scale rides in this file-static. Render is main-thread and the value is
 * set immediately before each call. */
static int s_clip_scale = 8;
static int chat_hud_clip_width(const char* text) {
	return gutil_font_width(text, s_clip_scale);
}

static void chat_hud_draw(const struct chat_ring_state* ring, int x, int base_y,
						  bool open, int max_lines, int strip_w, int clip_w) {
	if(!ring || max_lines <= 0)
		return;
	const int s = GFX_GUI_SCALE;
	bool bracket = false;
	for(size_t i = 0; i < ring->count && (int)i < max_lines; i++) {
		size_t idx = (ring->head + ring->count - 1 - i) % CHAT_RING_CAPACITY;
		int alpha = chat_line_alpha(ring->age[idx], open);
		if(alpha <= 0)
			continue;
		if(!bracket) {
			/* Faded glyphs need real blending (the slime-gel / name-tag
			 * bracket precedent); restored below. */
			gfx_blending(MODE_BLEND);
			gfx_alpha_test(false);
			bracket = true;
		}
		int y = base_y - (int)i * 9 * s;
		/* drawRect(2, y - 1, 322, y + 8, alpha / 2 << 24): a 320 x 9
		 * logical black strip under the text. */
		gfx_texture(false);
		gutil_texquad_col(x, y - 1 * s, 0, 0, 0, 0, strip_w, 9 * s, 0, 0, 0,
						  (uint8_t)(alpha / 2));
		gfx_texture(true);
		const char* text = ring->lines[idx];
		if(clip_w > 0 && gutil_font_width(text, 8 * s) > clip_w) {
			/* Drop whole trailing characters until the line fits the band; the
			 * stored ring line is never rewrapped or modified, and a Beta
			 * colour pair is never left half-cut (chat_clip_display). */
			char cut[CHAT_LINE_BYTES];
			s_clip_scale = 8 * s;
			chat_clip_display(text, cut, sizeof cut, clip_w, chat_hud_clip_width);
			gutil_text_rgba(x, y, cut, 8 * s, true, 0xFF, 0xFF, 0xFF,
							(uint8_t)alpha);
		} else {
			gutil_text_rgba(x, y, text, 8 * s, true, 0xFF, 0xFF, 0xFF,
							(uint8_t)alpha);
		}
	}
	if(bracket) {
		/* RESTORE THE PASS'S ENTRY STATE, WHICH IS MODE_BLEND + alpha test ON
		 * -- not MODE_OFF (review 28 H1). gfx_mode_gui_viewport enters every
		 * 2D pass at MODE_BLEND with the alpha test on, on both backends
		 * (source/platform/pc/gfx.c:412-413, source/platform/wii/gfx.c:
		 * 871-872), and this block is drawn INSIDE that pass -- the in-game
		 * HUD draws the hotbar, the sneak rail and Beta's sleep fade after
		 * it. Leaving MODE_OFF disabled blending for all of them on any frame
		 * where a chat line happened to be faded in; the alpha test was
		 * already restored correctly. screen_chat.c's own translucent
		 * rectangles each establish their own blend state (chat_rect), so the
		 * composer never depended on the wrong value either. */
		gfx_blending(MODE_BLEND);
		gfx_alpha_test(true);
	}
}

void chat_hud_render(const struct chat_ring_state* ring, int x, int base_y,
					 bool open, int max_lines) {
	chat_hud_draw(ring, x, base_y, open, max_lines, 320 * GFX_GUI_SCALE, 0);
}

void chat_hud_render_open(const struct chat_ring_state* ring, int x, int base_y,
						  int max_lines, int width) {
	chat_hud_draw(ring, x, base_y, true, max_lines, width, width);
}

int chat_hud_line_width(const char* text) {
	return text ? gutil_font_width(text, 8) : 0;
}

/* Build spec 15 A.3. Both are pure and live on the CALLER's side of the two
 * renderers above, which are unchanged -- so the composer's open band and a
 * single-player HUD emit exactly the bytes they emitted before.
 *
 * The 8 * GFX_GUI_SCALE is the newest line's text height (the same scale
 * chat_hud_draw passes gutil_text_rgba) and the second term is gutil_text's
 * hard shadow offset, which is `scale / 8` (gui_util.c gutil_text_rgba) --
 * NOT a fixed 2 px, as this comment used to claim (review 28 L1). The scale
 * passed here is 8 * GFX_GUI_SCALE, so the offset is GFX_GUI_SCALE itself: 1
 * at GUI scale 1, 2 at the shipped scale 2, 3 at scale 3. Written as the
 * scale rather than the 2 it currently evaluates to, so the helper stays
 * correct if GFX_GUI_SCALE ever moves. The line's black strip runs from
 * base_y - scale for 9 * scale and therefore ends one shadow-offset higher,
 * so the shadow is the lowest mark the block makes. */
int chat_hud_bottom(int base_y) {
	return base_y + 8 * GFX_GUI_SCALE + GFX_GUI_SCALE;
}

int chat_hud_clamp_base_y(int base_y, int bottom_limit) {
	int over = chat_hud_bottom(base_y) - bottom_limit;
	return over > 0 ? base_y - over : base_y;
}
