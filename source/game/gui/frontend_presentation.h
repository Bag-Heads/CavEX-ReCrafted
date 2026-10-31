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

#ifndef FRONTEND_PRESENTATION_H
#define FRONTEND_PRESENTATION_H

/* The approved front-end's DRAWING PRIMITIVES, and nothing else.
 *
 * Gate 2 (tmp/jobs/design-wiring-0922/GATE2-VISUAL.md) ports the committed
 * three-pack scenic composition -- front-end-refined/studio.js:77-85,
 * pack-style.js:14 and proofs/{beta,isabella,earth}--packs--default.png -- onto
 * the native screens. Everything the prototype expresses in CSS custom
 * properties is, natively, a crop of the ACTIVE PACK's own atlases:
 *
 *   --pack-paper   gui/crafting.png (150,62,16,16), tiled  -> fep_paper
 *   --pack-button  gui/gui.png      (0,66,200,20)          -> fep_widget(.., false)
 *   --pack-focus   gui/gui.png      (0,86,200,20)          -> fep_widget(.., true)
 *   --pack-backdrop gui/menu-background.png, centre-cover  -> fep_backdrop
 *
 * Those are the SAME rows Beta's own GuiButton reads [740c583
 * GuiButton.drawButton: disabled 46 / idle 66 / hover 86], so the material is
 * the pack's, never a theme guess: nothing here inspects a pack's NAME or
 * folder, and nothing here samples a palette to decide what a pack "is".
 *
 * RULES this header keeps (assignment 1):
 *   - static inline only, NO hidden state, no statics, no globals of its own;
 *   - no per-frame malloc and no per-frame I/O -- every draw is a fixed number
 *     of quads from already-loaded textures;
 *   - the real bitmap font, through the existing gutil_* entry points -- never
 *     a private glyph path;
 *   - a texture that did not load (optional asset, or a headless unit-test
 *     link double whose .data is NULL) degrades to the UNTEXTURED neutral base
 *     rather than sampling garbage UVs. Isabella's button rows are largely
 *     transparent in the approved source, which is exactly why the neutral base
 *     is painted UNDER the material rather than instead of it.
 *
 * It draws; it decides nothing. Layout, hit testing and state stay in the
 * screen that owns them.
 */

#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../platform/gfx.h"
#include "../../platform/texture.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The approved material crops, in TEXELS of the pack's own atlas. */
#define FEP_PAPER_U 150
#define FEP_PAPER_V 62
#define FEP_PAPER_S 16
#define FEP_WIDGET_U 0
#define FEP_WIDGET_IDLE_V 66
#define FEP_WIDGET_FOCUS_V 86
#define FEP_WIDGET_W 200
#define FEP_WIDGET_H 20

/* Neutral bases, painted under a (possibly transparent) pack material so every
 * control is opaque whatever the pack ships. Beta's own GUI greys. */
#define FEP_PAPER_BASE_R 0xC6
#define FEP_PAPER_BASE_G 0xC6
#define FEP_PAPER_BASE_B 0xC6
#define FEP_WIDGET_BASE_R 0x8B
#define FEP_WIDGET_BASE_G 0x8B
#define FEP_WIDGET_BASE_B 0x8B
/* The approved composition's PACK-ROW base, front-end-refined/studio.css
 * `.studio-pack { background: #383A37 }`. A row is not an ordinary button: it
 * is a dark card the pack's (in Isabella's case ENTIRELY transparent) button
 * row tints, so painting Beta's button grey under it showed #8B8B8B instead of
 * the approved card. This is an explicit ARGUMENT the screen passes, not an
 * inference -- nothing here looks at a pack's name or folder. */
#define FEP_ROW_BASE_R 0x38
#define FEP_ROW_BASE_G 0x3A
#define FEP_ROW_BASE_B 0x37

/* A rectangle in PHYSICAL pixels -- the one currency this header speaks. */
struct fep_rect {
	int x, y, w, h;
};

static inline struct fep_rect fep_rect(int x, int y, int w, int h) {
	struct fep_rect r = {x, y, w, h};
	return r;
}

/* Lower bound inclusive, upper exclusive -- gutil_button_hit's rule
 * [740c583 GuiButton.mousePressed], so a screen's own rects and the shared
 * button rects cannot disagree about an edge pixel. */
static inline bool fep_rect_hit(struct fep_rect r, float x, float y) {
	return r.w > 0 && r.h > 0 && x >= (float)r.x && x < (float)(r.x + r.w)
		&& y >= (float)r.y && y < (float)(r.y + r.h);
}

/* One untextured quad. Callers batch their own gfx_texture() bracket. */
static inline void fep_solid(struct fep_rect r, uint8_t cr, uint8_t cg,
							 uint8_t cb, uint8_t ca) {
	if(r.w <= 0 || r.h <= 0)
		return;
	gutil_texquad_col(r.x, r.y, 0, 0, 0, 0, r.w, r.h, cr, cg, cb, ca);
}

static inline void fep_fill(struct fep_rect r, uint8_t cr, uint8_t cg,
							uint8_t cb, uint8_t ca) {
	gfx_texture(false);
	fep_solid(r, cr, cg, cb, ca);
	gfx_texture(true);
}

/* Is this crop really inside a loaded texture? A missing optional asset and a
 * headless link double both present .data == NULL. */
static inline bool fep_material_ready(const struct tex_gfx* tex, int u, int v,
									  int w, int h) {
	return tex && tex->data && tex->width >= (size_t)(u + w)
		&& tex->height >= (size_t)(v + h);
}

/* The approved PAPER: the crafting atlas's 16x16 panel cell, tiled at GUI
 * scale over an opaque neutral base. Fixed quad count (ceil(w/tile) *
 * ceil(h/tile)), no allocation; the trailing partial tile crops its SOURCE in
 * proportion so the tile is never squashed. */
static inline void fep_paper(struct fep_rect r) {
	if(r.w <= 0 || r.h <= 0)
		return;
	fep_fill(r, FEP_PAPER_BASE_R, FEP_PAPER_BASE_G, FEP_PAPER_BASE_B, 0xFF);
	if(!fep_material_ready(&texture_gui_crafting, FEP_PAPER_U, FEP_PAPER_V,
						   FEP_PAPER_S, FEP_PAPER_S))
		return;
	int tile = FEP_PAPER_S * GFX_GUI_SCALE;
	gfx_bind_texture(&texture_gui_crafting);
	for(int y = 0; y < r.h; y += tile) {
		int th = r.h - y < tile ? r.h - y : tile;
		int tv = FEP_PAPER_S * th / tile;
		if(tv <= 0)
			continue;
		for(int x = 0; x < r.w; x += tile) {
			int tw = r.w - x < tile ? r.w - x : tile;
			int tu = FEP_PAPER_S * tw / tile;
			if(tu <= 0)
				continue;
			gutil_texquad(r.x + x, r.y + y, FEP_PAPER_U, FEP_PAPER_V, tu, tv,
						  tw, th);
		}
	}
}

/* The approved WIDGET surface: the pack's own button row, split into two
 * halves exactly like [740c583 GuiButton.drawButton] so the atlas's left and
 * right ends stay at the control's left and right ends at ANY width (a single
 * stretched quad would smear them into the middle). The 20-texel row is
 * stretched vertically -- an approved-composition control is 44 or 60 px tall,
 * not Beta's fixed 40 -- and the crisp edge is restored by fep_bevel, not by
 * the atlas. Idle row 66 / focus row 86, the rows Beta itself uses.
 *
 * The neutral base under the (possibly transparent) material is the CALLER's,
 * because the approved composition uses two of them: the button grey for an
 * ordinary control and the dark card #383A37 for a pack row. */
static inline void fep_widget_based(struct fep_rect r, bool focused,
									uint8_t br, uint8_t bg, uint8_t bb) {
	if(r.w <= 0 || r.h <= 0)
		return;
	fep_fill(r, br, bg, bb, 0xFF);
	int v = focused ? FEP_WIDGET_FOCUS_V : FEP_WIDGET_IDLE_V;
	if(!fep_material_ready(&texture_gui_widgets, FEP_WIDGET_U, v, FEP_WIDGET_W,
						   FEP_WIDGET_H))
		return;
	int half_tex = FEP_WIDGET_W / 2;
	int left = r.w / 2;
	gfx_bind_texture(&texture_gui_widgets);
	gutil_texquad(r.x, r.y, FEP_WIDGET_U, v, half_tex, FEP_WIDGET_H, left, r.h);
	gutil_texquad(r.x + left, r.y, FEP_WIDGET_U + half_tex, v, half_tex,
				  FEP_WIDGET_H, r.w - left, r.h);
}

/* An ordinary button: the same material over Beta's own button grey. */
static inline void fep_widget(struct fep_rect r, bool focused) {
	fep_widget_based(r, focused, FEP_WIDGET_BASE_R, FEP_WIDGET_BASE_G,
					 FEP_WIDGET_BASE_B);
}

/* A pack ROW: the same material over the approved card colour. */
static inline void fep_widget_row(struct fep_rect r, bool focused) {
	fep_widget_based(r, focused, FEP_ROW_BASE_R, FEP_ROW_BASE_G,
					 FEP_ROW_BASE_B);
}

/* A raised edge: light top/left, dark bottom/right, one GUI unit thick --
 * drawn INSIDE the rect so it never encroaches on a neighbour. */
static inline void fep_bevel(struct fep_rect r) {
	int e = GFX_GUI_SCALE;
	if(r.w <= 2 * e || r.h <= 2 * e)
		return;
	gfx_texture(false);
	fep_solid(fep_rect(r.x, r.y, r.w, e), 0xFF, 0xFF, 0xFF, 0x60);
	fep_solid(fep_rect(r.x, r.y + e, e, r.h - 2 * e), 0xFF, 0xFF, 0xFF, 0x60);
	fep_solid(fep_rect(r.x, r.y + r.h - e, r.w, e), 0x00, 0x00, 0x00, 0x70);
	fep_solid(fep_rect(r.x + r.w - e, r.y + e, e, r.h - 2 * e), 0x00, 0x00,
			  0x00, 0x70);
	gfx_texture(true);
}

/* An outline drawn INSIDE the rect: the FOCUS ring when bright, the "this row
 * is your pending / current choice" mark when it is not -- two visibly
 * different states on the same row, which is what the composition asks for.
 *
 * INSET, not outset: the approved front end draws this ring with
 * `outline-offset: -3px` (front-end-refined/studio.css), i.e. inside the
 * control's own box. An outset ring painted over the neighbouring row's
 * pixels -- with the approved row gap that is a visible encroachment, and on
 * the first/last row it left the list's bounds. Thickness is clamped to half
 * the rect so a ring can never invert or overdraw itself. */
static inline void fep_outline(struct fep_rect r, int thickness, uint8_t cr,
							   uint8_t cg, uint8_t cb, uint8_t ca) {
	if(thickness <= 0 || r.w <= 0 || r.h <= 0)
		return;
	int t = thickness;
	if(t > r.w / 2)
		t = r.w / 2;
	if(t > r.h / 2)
		t = r.h / 2;
	if(t <= 0)
		return;
	gfx_texture(false);
	fep_solid(fep_rect(r.x, r.y, r.w, t), cr, cg, cb, ca);
	fep_solid(fep_rect(r.x, r.y + r.h - t, r.w, t), cr, cg, cb, ca);
	fep_solid(fep_rect(r.x, r.y + t, t, r.h - 2 * t), cr, cg, cb, ca);
	fep_solid(fep_rect(r.x + r.w - t, r.y + t, t, r.h - 2 * t), cr, cg, cb, ca);
	gfx_texture(true);
}

/* THE SCENIC BACKDROP, stationary: the optional gui/menu-background.png filled
 * to the whole frame with a CENTRE-COVER crop (the shorter axis fills, the
 * longer one is cropped equally at both ends -- CSS `background-size: cover`,
 * which is what the prototype uses), then the dark wash the composition reads
 * its panel and text against.
 *
 * Returns false and draws NOTHING when the asset is absent, so the caller can
 * fall back to the ordinary tiled-dirt menu background. One quad + one wash
 * quad per frame; the owning screen supplies animation time.
 *
 * THE FIXED-256 UV CONTRACT -- read this before adding a caller. gutil_texquad's
 * tx/ty/sx/sy are NOT this texture's pixels: they are units of a FIXED 1/256
 * grid (gfx_draw_quads divides every texcoord by 256; the Wii backend's fixed
 * UVs agree). That works for the approved 256x256 GUI atlases only because
 * there 1 unit == 1 texel. menu-background.png is 1024x576, so passing its
 * pixel rect drove u,v past 1.0 and the clamp smeared the edge into stripes.
 * A non-256 texture must therefore go through gfx_draw_quads_flt with
 * genuinely NORMALIZED 0..1 source UVs, which is what this does -- same four
 * vertices at z = -2 and the same opaque white as gutil_texquad, just float. */
static inline bool fep_backdrop_at(int width, int height, uint8_t wash_alpha,
                                  float seconds, bool moving) {
	const struct tex_gfx* tex = &texture_menu_background;
	if(width <= 0 || height <= 0 || !tex->data || tex->width == 0
	   || tex->height == 0)
		return false;
	int tw = (int)tex->width, th = (int)tex->height;
	/* Match the approved overscan: 96px sides, 32px top/bottom at 480p.
	 * It leaves room to pan even when the image matches the display aspect. */
	float scale = (float)height / 480.0F;
	float canvas_w = width + (moving ? 192.0F * scale : 0.0F);
	float canvas_h = height + (moving ? 64.0F * scale : 0.0F);
	/* The largest source rect with the FRAME's aspect that fits the image. */
	int sw = tw, sh = th;
	if((float)tw * canvas_h > (float)th * canvas_w)
		sw = (int)(th * canvas_w / canvas_h);
	else
		sh = (int)(tw * canvas_h / canvas_w);
	if(sw <= 0 || sh <= 0)
		return false;
	int tx = (tw - sw) / 2, ty = (th - sh) / 2;
	float u0 = (float)tx / (float)tw, v0 = (float)ty / (float)th;
	float u1 = (float)(tx + sw) / (float)tw, v1 = (float)(ty + sh) / (float)th;
	if(moving) {
		float phase = fmodf(seconds, 128.0F);
		float pan = phase <= 64.0F ? phase / 64.0F : (128.0F - phase) / 64.0F;
		float shift = (-64.0F + 128.0F * pan) * scale;
		float du = u1 - u0, dv = v1 - v0;
		u0 += du * (96.0F * scale - shift) / canvas_w;
		u1 = u0 + du * width / canvas_w;
		v0 += dv * 32.0F * scale / canvas_h;
		v1 = v0 + dv * height / canvas_h;
	}
	float fw = (float)width, fh = (float)height;
	gfx_bind_texture((struct tex_gfx*)tex);
	gfx_draw_quads_flt(
		4,
		(float[]) {0.0F, 0.0F, -2.0F, fw, 0.0F, -2.0F, fw, fh, -2.0F, 0.0F, fh,
				   -2.0F},
		(uint8_t[]) {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
					 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
		(float[]) {u0, v0, u1, v0, u1, v1, u0, v1});
	if(wash_alpha)
		fep_fill(fep_rect(0, 0, width, height), 0, 0, 0, wash_alpha);
	return true;
}

/* Still pages retain their original centered crop. Moving pages explicitly
 * supply their presentation clock; this helper owns no hidden state. */
static inline bool fep_backdrop(int width, int height, uint8_t wash_alpha) {
	return fep_backdrop_at(width, height, wash_alpha, 0.0F, false);
}

/* Fit `src` into `room` PHYSICAL pixels of the REAL bitmap font at `size`,
 * ellipsising in the caller's buffer -- no allocation, same font, same glyphs,
 * only fewer of them. Returns true when the text had to be cut. */
static inline bool fep_text_fit(char* buf, size_t cap, const char* src,
								int size, int room) {
	if(!buf || cap == 0)
		return false;
	buf[0] = '\0';
	if(!src)
		return false;
	size_t len = strlen(src);
	/* cap - 1 characters plus the NUL fit EXACTLY, so only a LONGER source is
	 * cut by the buffer (the old `>=` reported a full-buffer string as
	 * truncated and then ellipsised a string that had lost nothing). */
	bool cut = len > cap - 1;
	size_t n = cut ? cap - 1 : len;
	memcpy(buf, src, n);
	buf[n] = '\0';
	if(!cut && gutil_font_width(buf, size) <= room)
		return false;
	/* Drop one character at a time until what remains fits `room` -- down to
	 * the EMPTY string. The old loop stopped at one character, so a room that
	 * is zero, or narrower than the first glyph, still drew a glyph OUTSIDE
	 * the control. "Nothing fits" must render nothing.
	 * The ellipsis is only attempted when it can actually be placed in the
	 * buffer AND something was really dropped; otherwise the bare remainder is
	 * measured, so a buffer too small for "..." still fits what it can. */
	size_t at = n;
	for(;;) {
		if(at + 4 <= cap && (cut || at < n)) {
			memcpy(buf + at, "...", 4);
			if(gutil_font_width(buf, size) <= room)
				return true;
			buf[at] = '\0';
		} else if(gutil_font_width(buf, size) <= room) {
			return true;
		}
		if(at == 0)
			return true; /* buf is "" -- no ink at all fits `room`. */
		buf[--at] = '\0';
	}
}

#endif
