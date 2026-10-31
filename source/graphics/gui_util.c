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

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../item/items.h" // direct include (was transitive via gui_util.h pre-split)
#include "../platform/controller_events.h"
#include "../platform/gfx.h"
#include "../platform/input_glyph.h"
#include "../platform/input_inventory.h"
#include "../platform/input_owner.h"
#include "gfx_settings.h"
#include "gui_util.h"
#include "render_block.h"
#include "texture_atlas.h"

void gutil_menu_focus_border(const struct gutil_button_bounds* bounds) {
	int edge = GFX_GUI_SCALE;
	int x = bounds->x - edge, y = bounds->y - edge;
	int width = bounds->width + 2 * edge;
	int height = bounds->height + 2 * edge;
	gfx_texture(false);
	gutil_texquad(x, y, 0, 0, 0, 0, width, edge);
	gutil_texquad(x, y + height - edge, 0, 0, 0, 0, width, edge);
	gutil_texquad(x, y + edge, 0, 0, 0, 0, edge, height - 2 * edge);
	gutil_texquad(x + width - edge, y + edge, 0, 0, 0, 0, edge,
				  height - 2 * edge);
	gfx_texture(true);
}

/* PC caps/mouse (cells 44-49) exist only in approved-controls.png; without
 * it they fall back to controls.png's generic KEY (42) / MSE (43) cells. */
static bool gutil_pc_cell(int cell) {
	return cell >= INPUT_GLYPH_CELL_KEY_1U
		&& cell <= INPUT_GLYPH_CELL_MOUSE_WHEEL;
}
static struct tex_gfx* gutil_glyph_texture(int cell) {
	return (cell < 42 || gutil_pc_cell(cell)) && texture_approved_controls.data ?
		&texture_approved_controls : &texture_controls;
}
/* The cell actually drawn: a PC cell on the fallback atlas becomes 42/43. */
static int gutil_glyph_drawn_cell(int cell) {
	if(!gutil_pc_cell(cell) || texture_approved_controls.data)
		return cell;
	return cell >= INPUT_GLYPH_CELL_MOUSE_LEFT ? 43 : 42;
}
/* The wide (64x32: cells 45+46) PC cap draws two cells wide. */
static bool gutil_glyph_wide(int cell) {
	return cell == INPUT_GLYPH_CELL_KEY_WIDE && texture_approved_controls.data;
}

int gutil_prompt_glyph_width(int glyph, int size) {
	int modifier, button;
	if(glyph < 0 || !input_glyph_cells(glyph, &modifier, &button))
		return 0;
	int w = gutil_glyph_wide(button) ? 2 * size : size;
	return modifier >= 0 ? size + size / 2 + w : w;
}

int gutil_prompt_glyph(int x, int y, int size, int glyph) {
	return gutil_prompt_glyph_gray(x, y, size, glyph, 255);
}

/* Classic shoulder keycaps (maintainer 10-31): approved-controls.png cells
 * 22/23/14/15 are BLANK caps and the letters follow the ACTIVE pack font.
 * See gui_util.h. */
const char* gutil_keycap_label(int cell) {
	/* one label table for every blank cap: input_glyph_label_for_code (a
	 * plain cell below 64 equals its code; PC caps go through the code) */
	return cell >= 0 && cell < INPUT_GLYPH_CELLS ?
		input_glyph_label_for_code(cell, NULL) : NULL;
}

bool gutil_keycap_label_rect(int cell, int size, int* x, int* y, int* w,
							 int* h) {
	const char* label = gutil_keycap_label(cell);
	if(!label || size <= 0)
		return false;
	int scale = size / 2;
	/* gutil_font_width's advance ends with one font unit of spacing: the
	 * gap between letters (2 px at a 32 px glyph); the last one is not ink */
	*w = gutil_font_width(label, scale) - scale / 8;
	*h = 7 * scale / 8; /* Beta's 7-row letters */
	*x = (size - *w) / 2;
	*y = (int)floorf(size * 14.1F / 32.0F - *h / 2.0F + 0.5F);
	return true;
}

/* PC caps (ctl1031-pc-glyphs style A, 10-31): the key NAME on the blank 1u or
 * wide cap, in the active pack font, same placement as the Classic keycaps
 * (size/2 text, centred, vertical centre 14.1/32); a name wider than the cap
 * face (cap width - 10/32 of the size) drops to the 1x font (size/4). */
bool gutil_pc_cap_label_rect(const char* label, int cap_w, int size, int* x,
							 int* y, int* w, int* h, int* scale) {
	if(!label || !label[0] || size <= 0 || cap_w <= 0)
		return false;
	*scale = size / 2;
	*w = gutil_font_width(label, *scale) - *scale / 8;
	if(*w > cap_w - 10 * size / 32) {
		*scale = size / 4;
		*w = gutil_font_width(label, *scale) - *scale / 8;
	}
	*h = 7 * *scale / 8;
	*x = (cap_w - *w) / 2;
	*y = (int)floorf(size * 14.1F / 32.0F - *h / 2.0F + 0.5F);
	return true;
}

static void gutil_keycap_overlay(int x, int y, int size, int cell,
								 uint8_t gray) {
	int lx, ly, lw, lh;
	if(gutil_glyph_texture(cell) != &texture_approved_controls
	   || !gutil_keycap_label_rect(cell, size, &lx, &ly, &lw, &lh))
		return; /* the fallback controls.png keeps its own lettered cells */
	uint8_t ink = (uint8_t)(20 * gray / 255);
	gutil_text_rgb(x + lx, y + ly, gutil_keycap_label(cell), size / 2, false,
				   ink, ink, ink);
}

/* One atlas cell (or the wide 2-cell cap) at x,y; returns the drawn width. */
static int gutil_glyph_cell(int x, int y, int size, int cell, uint8_t gray) {
	int drawn = gutil_glyph_drawn_cell(cell);
	bool wide = gutil_glyph_wide(cell);
	gfx_bind_texture(gutil_glyph_texture(cell));
	gutil_texquad_col(x, y, (drawn % 8) * 32, (drawn / 8) * 32, wide ? 64 : 32,
					  32, wide ? 2 * size : size, size, gray, gray, gray, 255);
	return wide ? 2 * size : size;
}

int gutil_prompt_glyph_gray(int x, int y, int size, int glyph, uint8_t gray) {
	int modifier, button;
	if(!input_glyph_cells(glyph, &modifier, &button))
		return 0;
	int start = x;
	if(modifier < 0 && gutil_pc_cell(button)) {
		/* a PC glyph code: the cap (or mouse) and, on a cap, the key name */
		int w = gutil_glyph_cell(x, y, size, button, gray);
		int lx, ly, lw, lh, scale;
		const char* label = texture_approved_controls.data ?
			input_glyph_label_for_code(glyph, NULL) : NULL;
		if(gutil_pc_cap_label_rect(label, w, size, &lx, &ly, &lw, &lh, &scale)) {
			uint8_t ink = (uint8_t)(20 * gray / 255);
			gutil_text_rgb(x + lx, y + ly, label, scale, false, ink, ink, ink);
		}
		return w;
	}
	if(modifier >= 0) {
		gfx_bind_texture(gutil_glyph_texture(modifier));
		gutil_texquad_col(x, y, (modifier % 8) * 32, (modifier / 8) * 32, 32, 32,
					  size, size, gray, gray, gray, 255);
		gutil_keycap_overlay(x, y, size, modifier, gray);
		x += size;
		gutil_text_rgb(x, y + size / 4, "+", size / 2, true, gray, gray, gray);
		x += size / 2;
	}
	gfx_bind_texture(gutil_glyph_texture(button));
	gutil_texquad_col(x, y, (button % 8) * 32, (button / 8) * 32, 32, 32, size,
				  size, gray, gray, gray, 255);
	gutil_keycap_overlay(x, y, size, button, gray);
	return x + size - start;
}

/* Maintainer-approved October 28 sheet: native large and mini cells. */
int gutil_controller_picture_width(int picture, int height) {
	int width;
	switch(picture) {
		case CONTROLLER_PICTURE_REMOTE: width = 24; break;
		case CONTROLLER_PICTURE_NUNCHUK: width = 56; break;
		case CONTROLLER_PICTURE_GAMECUBE: width = 96; break;
		case CONTROLLER_PICTURE_CLASSIC: width = 120; break;
		default: return 0;
	}
	return height >= 48 ? width * height / 64 : 40 * height / 24;
}

void gutil_controller_picture(int x, int y, int width, int height, int picture) {
	int sx, sy, sw, sh;
	if(height >= 48) {
		sy = 4; sh = 64;
		sw = gutil_controller_picture_width(picture, 64);
		switch(picture) {
			case CONTROLLER_PICTURE_REMOTE: sx = 4; break;
			case CONTROLLER_PICTURE_NUNCHUK: sx = 36; break;
			case CONTROLLER_PICTURE_GAMECUBE: sx = 100; break;
			case CONTROLLER_PICTURE_CLASSIC: sx = 4; sy = 76; break;
			default: return;
		}
	} else {
		sw = 40; sh = 24;
		switch(picture) {
			case CONTROLLER_PICTURE_REMOTE: sx = 132; sy = 76; break;
			case CONTROLLER_PICTURE_NUNCHUK: sx = 180; sy = 76; break;
			case CONTROLLER_PICTURE_GAMECUBE: sx = 132; sy = 108; break;
			case CONTROLLER_PICTURE_CLASSIC: sx = 180; sy = 108; break;
			default: return;
		}
	}
	if(!texture_controller_pictures.data || width <= 0 || height <= 0)
		return;
	gfx_bind_texture(&texture_controller_pictures);
	gutil_texquad_col(x, y, sx, sy, sw, sh, width, height, 255, 255, 255, 255);
}

/* ---- the join glyph: ONE detected-device identity (Astra pack-0926 #1) ----
 * The title's "Press A to join" and the approved Join card both draw the
 * OFFERED device's own bundled A cell -- Remote/Nunchuk 0, Classic 10,
 * GameCube 32 (menus.html:922) -- and both derive it here, from the
 * inventory's PUBLISHED (debounced) identity of a real device. Nothing is
 * inferred from "unassigned": no detected eligible device means no glyph at
 * all (the title keeps its text, without icon spacing) and no actionable
 * card. The old pack-font "[A]" stand-in is gone: a font glyph differs per
 * pack and is not approved art. The cells live on gui/approved-controls.png
 * (controls.png as the cell fallback), both bundled-only in texture.c. */
static const struct input_inventory* gutil_inventory_override;

void gutil_join_inventory_override(const struct input_inventory* inventory) {
	gutil_inventory_override = inventory;
}

static const struct input_inventory* gutil_inventory(void) {
	return gutil_inventory_override ? gutil_inventory_override :
		input_devices();
}

static int gutil_family_confirm_cell(struct input_device device,
									 enum input_category family) {
	if(family == INPUT_CAT_GAMECUBE)
		return 32;
	if(device.kind == INPUT_DEVICE_GAMECUBE)
		return -1; /* a GameCube port never publishes another family */
	if(family == INPUT_CAT_CLASSIC_CONTROLLER)
		return 10;
	if(family == INPUT_CAT_NUNCHUK)
		return 0;
	return -1; /* bare Remote / none / keyboard: not an eligible device */
}

/* The A cell of `device` if the inventory currently publishes it as a
 * connected, eligible controller; -1 otherwise. */
static int gutil_detected_confirm_cell(const struct input_inventory* inventory,
									   struct input_device device,
									   bool require_unassigned) {
	if(!inventory || device.kind == INPUT_DEVICE_NONE)
		return -1;
	const struct input_device_status* status
		= input_inventory_device(inventory, device);
	if(!status || !status->published.connected
	   || (require_unassigned && status->published_assigned))
		return -1;
	return gutil_family_confirm_cell(device, status->published.category);
}

int gutil_join_offer_cell(const struct input_inventory* inventory) {
	if(!inventory)
		return -1;
	/* The inventory's own join offer names THE offered device... */
	if(inventory->join.state == INPUT_JOIN_OFFERED) {
		int cell = gutil_detected_confirm_cell(inventory, inventory->join.device,
											   true);
		if(cell >= 0)
			return cell;
	}
	/* ...and when that offer lapsed (its device left or was assigned) while
	 * another eligible device stays connected, the offer hands over to the
	 * first such device in port order: Wii Remote channels, then GameCube. */
	static const enum input_device_kind kinds[2]
		= {INPUT_DEVICE_WPAD, INPUT_DEVICE_GAMECUBE};
	for(int k = 0; k < 2; k++)
		for(unsigned channel = 0; channel < 4; channel++) {
			int cell = gutil_detected_confirm_cell(
				inventory, (struct input_device) {kinds[k], channel}, true);
			if(cell >= 0)
				return cell;
		}
	return -1;
}

/* Player 1 holds a controller: some device's published assignment is player
 * 1 (kept while that device is away -- the owner stays bound). Until then the
 * only join is player 1's own ("Press A to join"), so a detected device's
 * card offers no "Join P2" (Astra glyph visual review 2026-09-26). */
static bool gutil_player_one_assigned(const struct input_inventory* inventory) {
	for(size_t k = 0; inventory && k < INPUT_DEVICE_COUNT; k++)
		if(inventory->devices[k].published_assigned
		   && inventory->devices[k].published_player == 0)
			return true;
	return false;
}

/* The title's join glyph: the offered device's A cell, or nothing. Returns
 * the drawn width (0 when there is no detected eligible device). */
static int gutil_join_glyph(int x, int y, int size) {
	int cell = gutil_join_offer_cell(gutil_inventory());
	return cell >= 0 ? gutil_prompt_glyph(x, y, size, cell) : 0;
}
static int gutil_join_glyph_width(int size) {
	return gutil_join_offer_cell(gutil_inventory()) >= 0 ? size : 0;
}

/* Glyph-to-label gap of the approved INVENTORY/MACHINE footers: 3 px, 6 px
 * for Close, which the design draws as a button with its own internal gap
 * (menus.html:29/33/35, Astra ui1). Every other prompt -- chest footers and
 * Sort (chests.html:12), options, video settings, sign controls -- keeps the
 * 4 px gap (GUTIL_MENU_HINT_GAP); Astra ui2 scoped it so. */
int gutil_footer_hint_gap(enum input_button b) {
	return b == IB_INVENTORY ? 6 : 3;
}

/* The width a menu hint DRAWS with the given glyph-to-label gap, so footers
 * can space and centre on measured prompts: a chord (modifier + "+" +
 * button) is size + size/2 + size = 80 px at the 32 px icon, not the 32 px
 * the old fixed 36/48 stepping assumed. */
int gutil_menu_hint_width_gap(const struct input_player_frame* input,
							  enum input_button b, const char* text, int gap) {
	int icon_size = 16 * GFX_GUI_SCALE;
	int text_size = 8 * GFX_GUI_SCALE;
	int glyph = 0;
	if(input && input->join_prompt) {
		if(b != IB_GUI_CLICK)
			return 0;
		text = "Press A to join";
		glyph = gutil_join_glyph_width(icon_size); /* = gutil_join_glyph */
		if(!glyph) /* no device: the text alone, no icon spacing */
			return gutil_font_width(text, text_size);
	} else if(input && b >= 0 && b < IB_COUNT) {
		glyph = gutil_prompt_glyph_width(input->glyphs[b], icon_size);
	}
	return glyph + gap + gutil_font_width(text, text_size);
}
int gutil_menu_hint_width_owned(const struct input_player_frame* input,
								enum input_button b, const char* text) {
	return gutil_menu_hint_width_gap(input, b, text, GUTIL_MENU_HINT_GAP);
}

void gutil_menu_hint_gap(const struct input_player_frame* input, int x, int y,
						 enum input_button b, const char* text, int gap) {
	if(input && input->join_prompt) {
		if(b != IB_GUI_CLICK)
			return;
		text = "Press A to join";
	}
	int icon_size = 16 * GFX_GUI_SCALE;
	int text_size = 8 * GFX_GUI_SCALE;
	int width = input && input->join_prompt ?
		gutil_join_glyph(x, y, icon_size) :
		input && b >= 0 && b < IB_COUNT ?
		gutil_prompt_glyph(x, y, icon_size, input->glyphs[b]) :
		0;
	if(input && input->join_prompt && !width)
		gap = 0; /* no detected device: the text alone, no icon spacing */
	gutil_text(x + width + gap, y + (icon_size - text_size) / 2, text, text_size,
			   true);
}

void gutil_menu_hint_owned(const struct input_player_frame* input, int x, int y,
						   enum input_button b, const char* text) {
	gutil_menu_hint_gap(input, x, y, b, text, GUTIL_MENU_HINT_GAP);
}

void gutil_menu_hint_player(unsigned player, int x, int y, enum input_button b,
	const char* text) {
	struct input_player_frame input;
	input_prompt_frame(player, &input);
	gutil_menu_hint_owned(&input, x, y, b, text);
}

/* Player 1's global screens; split panes call the _player/_owned forms. */
void gutil_menu_hint(int x, int y, enum input_button b, const char* text) {
	gutil_menu_hint_player(0, x, y, b, text);
}

/* ---- the hint row (see gui_util.h for the contract) ---------------------- */
int gutil_hint_row_margin(int width) {
	return width > 0 ? (width * 5 + 99) / 100 : 0;
}

/* Width gutil_prompt_glyph draws for `glyph` at `size`; 0 when undrawable. */
static int hint_glyph_width(int glyph, int size) {
	return gutil_prompt_glyph_width(glyph, size); /* wide PC caps: 2x */
}

/* Measures items[k] into `cell` with content starting at x = 0. Returns the
 * item's width, 0 when none of its roles has a drawable glyph. */
static int hint_measure(const struct input_player_frame* input,
						const struct gutil_hint_item* item, int k,
						struct gutil_hint_cell* cell) {
	memset(cell, 0, sizeof(*cell));
	cell->item = k;
	cell->label = item->label ? item->label : "";
	int roles = item->role_count == 2 ? 2 : 1, at = 0;
	for(int r = 0; r < roles; r++) {
		enum input_button b = item->roles[r];
		if(!input || b < 0 || b >= IB_COUNT)
			continue;
		int w = hint_glyph_width(input->glyphs[b], GUTIL_HINT_GLYPH);
		if(!w)
			continue;
		if(cell->glyph_count)
			at += GUTIL_HINT_PAIR_GAP;
		cell->glyph[cell->glyph_count] = input->glyphs[b];
		cell->glyph_x[cell->glyph_count] = at;
		cell->glyph_w[cell->glyph_count] = w;
		cell->glyph_count++;
		at += w;
	}
	if(!cell->glyph_count)
		return 0;
	cell->label_w = gutil_font_width(cell->label, GUTIL_HINT_TEXT);
	cell->label_x = at + GUTIL_HINT_LABEL_GAP;
	return cell->label_x + cell->label_w;
}

bool gutil_hint_row_layout(const struct input_player_frame* input, int x,
						   int y, int width, int height, bool split,
						   enum gutil_hint_align align,
						   const struct gutil_hint_item* items, int count,
						   struct gutil_hint_row* out) {
	memset(out, 0, sizeof(*out));
	if(!items || count <= 0 || width <= 0 || height <= 0)
		return false;
	if(count > GUTIL_HINT_ROW_MAX)
		count = GUTIL_HINT_ROW_MAX;
	struct gutil_hint_cell cells[GUTIL_HINT_ROW_MAX];
	int widths[GUTIL_HINT_ROW_MAX], n = 0;
	for(int k = 0; k < count; k++) {
		int w = hint_measure(input, &items[k], k, &cells[n]);
		if(w > 0)
			widths[n++] = w;
	}
	if(!n)
		return false;
	int margin = gutil_hint_row_margin(width);
	int room = width - 2 * margin - 2 * GUTIL_HINT_PAD;
	int gap;
	for(;;) {
		int sum = 0;
		for(int k = 0; k < n; k++)
			sum += widths[k];
		if(sum + (n - 1) * GUTIL_HINT_GAP <= room) {
			gap = GUTIL_HINT_GAP;
			out->fits = true;
			break;
		}
		if(sum + (n - 1) * GUTIL_HINT_GAP_TIGHT <= room) {
			gap = GUTIL_HINT_GAP_TIGHT;
			out->fits = true;
			break;
		}
		/* drop the lowest priority droppable item; ties: the rightmost */
		int drop = -1;
		for(int k = 0; k < n; k++)
			if(!items[cells[k].item].keep
			   && (drop < 0 || items[cells[k].item].priority
								   <= items[cells[drop].item].priority))
				drop = k;
		if(drop < 0) {
			gap = GUTIL_HINT_GAP_TIGHT;
			out->fits = false;
			break;
		}
		for(int k = drop; k + 1 < n; k++) {
			cells[k] = cells[k + 1];
			widths[k] = widths[k + 1];
		}
		n--;
	}
	int content = 0;
	for(int k = 0; k < n; k++)
		content += widths[k];
	content += (n - 1) * gap;
	int strip_w = content + 2 * GUTIL_HINT_PAD;
	int bottom = y + height
		- (split ? GUTIL_HINT_BOTTOM_SPLIT : GUTIL_HINT_BOTTOM_FULL);
	int strip_x = align == GUTIL_HINT_ALIGN_CENTRE || !out->fits ?
		x + (width - strip_w) / 2 : x + margin;
	out->strip = (struct gutil_button_bounds) {
		strip_x, bottom - GUTIL_HINT_STRIP_H, strip_w, GUTIL_HINT_STRIP_H};
	out->glyph_y = out->strip.y + (GUTIL_HINT_STRIP_H - GUTIL_HINT_GLYPH) / 2;
	/* one centre line: Beta's letters are 7 rows (14 px at 16) tall */
	out->text_y = out->glyph_y + GUTIL_HINT_GLYPH / 2 - 7;
	out->gap = gap;
	out->count = n;
	int at = strip_x + GUTIL_HINT_PAD;
	for(int k = 0; k < n; k++) {
		struct gutil_hint_cell* c = &out->cells[k];
		*c = cells[k];
		for(int g = 0; g < c->glyph_count; g++)
			c->glyph_x[g] += at;
		c->label_x += at;
		c->rect = (struct gutil_button_bounds) {
			at, out->strip.y, widths[k], GUTIL_HINT_STRIP_H};
		at += widths[k] + gap;
	}
	return true;
}

int gutil_hint_row_hit(const struct gutil_hint_row* row, float px, float py) {
	for(int k = 0; row && k < row->count; k++)
		if(gutil_button_hit(&row->cells[k].rect, px, py))
			return row->cells[k].item;
	return -1;
}

void gutil_hint_row_draw(const struct gutil_hint_row* row) {
	if(!row || row->count <= 0 || row->strip.width <= 0)
		return;
	/* No backing strip (maintainer 2026-10-31: it read as a grey bar on the
	 * TV). The strip rect still bounds layout and hit tests; glyphs and
	 * shadowed labels draw directly over the scene. The caller's blend state
	 * is not guaranteed here, so set it for the glyph alpha. */
	gfx_blending(MODE_BLEND);
	gfx_texture(true);
	for(int k = 0; k < row->count; k++) {
		const struct gutil_hint_cell* c = &row->cells[k];
		for(int g = 0; g < c->glyph_count; g++)
			gutil_prompt_glyph(c->glyph_x[g], row->glyph_y, GUTIL_HINT_GLYPH,
							   c->glyph[g]);
		gutil_text(c->label_x, row->text_y, c->label, GUTIL_HINT_TEXT, true);
	}
}

/* Approved fixed-size notice artwork. One newest live card per owner. */
static void notice_solid(int x,int y,int w,int h,uint8_t r,uint8_t g,uint8_t b) {
 if(w>0&&h>0)gutil_texquad_col(x,y,0,0,0,0,w,h,r,g,b,255);
}
/* Approved battery glyph: 24x12 body (2 px outline, nub in the 4 px right
 * margin, a low-charge cell inside) -- menus.html:96, Astra ui1. On the
 * Beta-grey card A (10-31) the outline is dark (32) instead of white. */
static void notice_battery(int x,int y,uint8_t o) {
 gfx_texture(false);
 notice_solid(x,y,24,2,o,o,o);notice_solid(x,y+10,24,2,o,o,o);
 notice_solid(x,y+2,2,8,o,o,o);notice_solid(x+22,y+2,2,8,o,o,o);
 notice_solid(x+24,y+4,3,4,o,o,o);notice_solid(x+4,y+4,4,4,215,152,75);
 gfx_texture(true);
}
struct notice_layout {
 struct controller_card card;
 int confirm,cancel,font,pad,height,picture_width,picture_height,glyph_size,confirm_width,cancel_width,fixed,width,gap,label_gap;
};
static int notice_action_width(const char* label,int glyph,int font,int size,int label_gap) {
 int gw=gutil_prompt_glyph_width(glyph,size);
 return label[0]&&gw?gw+label_gap+gutil_font_width(label,font):0;
}
static bool s_notice_menu_ctx; /* toast-1030, set by gutil_notices_menu_context */
static bool notice_layout(unsigned player,bool split,bool header,bool mini,struct notice_layout* l) {
 const struct input_inventory* inventory=gutil_inventory();if(!inventory)return false;
 ptime_t now=time_get();const struct input_connection_notice* notice=NULL;
 for(size_t k=0;k<INPUT_NOTICE_COUNT;k++) {
  const struct input_connection_notice* n=&inventory->notices[k];
  if(!input_notice_visible(n,player,now))continue;
  if(s_notice_menu_ctx&&n->event==CONTROLLER_EVENT_POINTER_LOST)continue; /* #02: menus have the D-pad */
  if(!notice||time_reached(n->expires,notice->expires))notice=n;
 }
 if(!notice||!controller_card_build(notice,player,split,now,0,0,640,480,&l->card))return false;
 enum input_category family=notice->connected?notice->category:notice->previous_category;
 if(family!=INPUT_CAT_NUNCHUK&&family!=INPUT_CAT_CLASSIC_CONTROLLER&&family!=INPUT_CAT_GAMECUBE) {
  const struct input_device_status* device=input_inventory_device(inventory,input_notice_device(notice));
  if(device)family=device->published.category;
 }
 controller_card_name_join_target(&l->card,inventory);
 if(l->card.picture!=CONTROLLER_PICTURE_NONE&&l->card.picture!=CONTROLLER_PICTURE_BATTERY)
  l->card.picture=controller_picture_of(input_notice_device(notice),family);
 if((notice->event==CONTROLLER_EVENT_DETECTED||notice->event==CONTROLLER_EVENT_JOIN_CANCELLED)
    &&!gutil_player_one_assigned(inventory))
  l->card.action_confirm[0]='\0'; /* no P2 join before player 1 exists */
 /* An ACTIONABLE card (Join / Retry / Cancel) belongs to a device that is
  * here now: its A/B cells come from that device's published identity -- the
  * same derivation as the title's join glyph -- and without a detected
  * eligible device the card is suppressed, never drawn with a guessed family. */
 if(l->card.action_confirm[0]||l->card.action_cancel[0]) {
  l->confirm=gutil_detected_confirm_cell(inventory,input_notice_device(notice),false);
  if(l->confirm<0)return false;
 } else l->confirm=-1;
 l->cancel=l->confirm>=0?l->confirm+1:-1;
 /* Card A (maintainer pick 10-31, opus_variants.py card()): Beta grey 198,
  * 32 tall, pad 8, 16 px grey-40 text, 24 px mini picture / glyph / battery,
  * children 8 apart, every child on the card's one centre line. The compact
  * in-header and mini strips keep their approved sizes. */
 bool card=!header&&!mini;
 l->font=card?GUTIL_NOTICE_TEXT:12;l->pad=header?0:mini?6:8;l->height=mini?16:header?24:GUTIL_NOTICE_CARD_H;
 l->picture_height=mini?16:header?20:24;l->glyph_size=mini?16:24;
 l->gap=card?8:6;l->label_gap=card?4:2;
 l->picture_width=l->card.picture==CONTROLLER_PICTURE_BATTERY?(card?27:28):l->card.picture==CONTROLLER_PICTURE_NONE?0:mini?26:header?32:40;
 l->confirm_width=notice_action_width(l->card.action_confirm,l->confirm,l->font,l->glyph_size,l->label_gap);
 l->cancel_width=notice_action_width(l->card.action_cancel,l->cancel,l->font,l->glyph_size,l->label_gap);
 int children=1+(l->picture_width>0)+(l->confirm_width>0)+(l->cancel_width>0);
 l->fixed=l->pad*2+l->picture_width+l->confirm_width+l->cancel_width+(children-1)*l->gap;
 l->width=l->fixed+gutil_font_width(l->card.text,l->font);return true;
}
#define GUTIL_TEXT_LOG 160
static bool s_text_log_on;
static int s_text_log_n;
static struct { int x, y, w, h; } s_text_log[GUTIL_TEXT_LOG];

void gutil_text_log_begin(void) {
	s_text_log_on = true;
	s_text_log_n = 0;
}
void gutil_text_log_end(void) { s_text_log_on = false; }
void gutil_keepout(int x, int y, int w, int h) {
	if(!s_text_log_on || s_text_log_n >= GUTIL_TEXT_LOG || w <= 0 || h <= 0)
		return;
	s_text_log[s_text_log_n].x = x;
	s_text_log[s_text_log_n].y = y;
	s_text_log[s_text_log_n].w = w;
	s_text_log[s_text_log_n].h = h;
	s_text_log_n++;
}
void gutil_notices_menu_context(bool menu) { s_notice_menu_ctx = menu; }

int gutil_notice_spot_hits(int x, int y, int w, int h) {
	const int m = 4;
	int hits = 0;
	for(int k = 0; k < s_text_log_n; k++) {
		if(x - m < s_text_log[k].x + s_text_log[k].w
		   && s_text_log[k].x < x + w + m
		   && y - m < s_text_log[k].y + s_text_log[k].h
		   && s_text_log[k].y < y + h + m)
			hits++;
	}
	return hits;
}

bool gutil_notice_free_spot(int width, int height, int card_w, int card_h,
	int* x, int* y) {
	/* notice-spot-1031 (maintainer 10-31): ONE anchor, top-left, the same
	 * (24, 16) gameplay uses (design A, 10-31). A heading under it nudges the card DOWN to just
	 * below that heading, same left edge; the nudge stays in the top band. It
	 * never jumps to another corner or edge (the toast-1030 corner search put
	 * the title's card bottom-left: its logo keep-out covered both top
	 * corners). If no in-band spot clears the content, the anchor itself is
	 * used and false is returned. */
	(void)width;
	const int m = 4, left = GUTIL_NOTICE_ANCHOR_X, top = GUTIL_NOTICE_ANCHOR_Y;
	const int band = height / 4;
	*x = left;
	*y = top;
	for(int pass = 0; pass < GUTIL_TEXT_LOG; pass++) {
		int below = -1;
		for(int k = 0; k < s_text_log_n; k++) {
			if(left - m < s_text_log[k].x + s_text_log[k].w
			   && s_text_log[k].x < left + card_w + m
			   && *y - m < s_text_log[k].y + s_text_log[k].h
			   && s_text_log[k].y < *y + card_h + m) {
				int b = s_text_log[k].y + s_text_log[k].h + m;
				if(b > below)
					below = b;
			}
		}
		if(below < 0)
			return true;
		if(below + card_h > band)
			break;
		*y = below;
	}
	*y = top;
	return false;
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* Headless suites: pretend a notice of this width is up (0 = real state). */
static int s_test_notice_width;
void gutil_test_force_notice_width(int width);
void gutil_test_force_notice_width(int width) { s_test_notice_width = width; }
#endif
int gutil_connection_notice_width(unsigned player,bool split,bool header) {
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
 if(s_test_notice_width>0&&!header)return s_test_notice_width;
#endif
 struct notice_layout l;return notice_layout(player,split,header,false,&l)?l.width:0;
}
static void notice_fit(char out[48],const char* src,int font,int width) {
 snprintf(out,48,"%s",src);
 if(gutil_font_width(out,font)<=width)return;
 int dots=gutil_font_width("...",font);if(width<dots){out[0]='\0';return;}
 size_t n=strlen(out);while(n&&gutil_font_width(out,font)>width-dots)out[--n]='\0';
 if(n+3<48)memcpy(out+n,"...",4);
}
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* notice-spot-1031 stress: the last card notice_draw painted. Card A (10-31)
 * appends every child's drawn box (picture/battery, text ink, glyphs, action
 * label ink) so a test can check the one-centre-line rule. */
struct gutil_test_notice_draw { int x, y, w, h; char text[48];
 int children; struct { int x, y, w, h; } child[8]; };
struct gutil_test_notice_draw gutil_test_last_notice;
static void notice_child(int x,int y,int w,int h) {
 if(gutil_test_last_notice.children<8)
  gutil_test_last_notice.child[gutil_test_last_notice.children++].x=x,
  gutil_test_last_notice.child[gutil_test_last_notice.children-1].y=y,
  gutil_test_last_notice.child[gutil_test_last_notice.children-1].w=w,
  gutil_test_last_notice.child[gutil_test_last_notice.children-1].h=h;
}
#else
#define notice_child(x,y,w,h) ((void)0)
#endif
/* Text whose letters' ink (7 of 8 font rows) is centred on `cy`. The compact
 * strips keep their historic (size-font)/2 placement. */
static int notice_text_y(int y,int height,int font,bool card) {
 return card?y+height/2-font*7/16:y+(height-font)/2;
}
static int notice_action(int x,int y,int height,const char* label,int glyph,int font,uint8_t ink,int size,int label_gap,bool card) {
 int gy=y+(height-size)/2;
 int used=gutil_prompt_glyph(x,gy,size,glyph);notice_child(x,gy,used,size);x+=used+label_gap;
 int ty=notice_text_y(y,height,font,card);
 gutil_text_rgb(x,ty,label,font,false,ink,ink,ink);
 notice_child(x,ty,gutil_font_width(label,font),font*7/8);
 return x+gutil_font_width(label,font);
}
static void notice_draw(unsigned player,bool split,int x,int y,int width,int height,bool header,bool mini) {
 struct notice_layout l;if(!notice_layout(player,split,header,mini,&l)||height<l.height||width<l.fixed)return;
 char text[48];notice_fit(text,l.card.text,l.font,width-l.fixed);
 int text_width=gutil_font_width(text,l.font),card_width=l.fixed+text_width;
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
 memset(&gutil_test_last_notice,0,sizeof(gutil_test_last_notice));
 gutil_test_last_notice.x=x;gutil_test_last_notice.y=y;
 gutil_test_last_notice.w=card_width;gutil_test_last_notice.h=l.height;
 snprintf(gutil_test_last_notice.text,sizeof(gutil_test_last_notice.text),"%s",l.card.text);
#endif
 bool card=!header&&!mini;
 uint8_t ink=header?64:card?GUTIL_NOTICE_INK:255;
 if(card) {
  /* Beta button look: 2 px near-black edge outside the card, grey 198 body,
   * 2 px light (white at alpha 153) top/left, 2 px dark (85) bottom/right */
  gfx_blending(MODE_BLEND);gfx_texture(false);
  notice_solid(x-2,y-2,card_width+4,l.height+4,17,17,17);
  notice_solid(x,y,card_width,l.height,198,198,198);
  gutil_texquad_col(x,y,0,0,0,0,card_width,2,255,255,255,153);
  gutil_texquad_col(x,y,0,0,0,0,2,l.height,255,255,255,153);
  notice_solid(x,y+l.height-2,card_width,2,85,85,85);
  notice_solid(x+card_width-2,y,2,l.height,85,85,85);
  gfx_texture(true);
 } else if(!header) {
  gfx_texture(false);notice_solid(x,y,card_width,l.height,44,43,39);
  notice_solid(x,y,card_width,1,148,145,138);notice_solid(x,y,1,l.height,148,145,138);
  notice_solid(x,y+l.height-1,card_width,1,21,20,18);notice_solid(x+card_width-1,y,1,l.height,21,20,18);gfx_texture(true);
 }
 int at=x+l.pad;
 if(l.picture_width) {
  if(l.card.picture==CONTROLLER_PICTURE_BATTERY) {
   notice_battery(at,y+(l.height-12)/2,card?32:255);notice_child(at,y+(l.height-12)/2,27,12);
  } else {
   gutil_controller_picture(at,y+(l.height-l.picture_height)/2,l.picture_width,l.picture_height,l.card.picture);
   notice_child(at,y+(l.height-l.picture_height)/2,l.picture_width,l.picture_height);
  }
  at+=l.picture_width+l.gap;
 }
 int ty=notice_text_y(y,l.height,l.font,card);
 gutil_text_rgb(at,ty,text,l.font,false,ink,ink,ink);notice_child(at,ty,text_width,l.font*7/8);at+=text_width;
 if(l.confirm_width)at=notice_action(at+l.gap,y,l.height,l.card.action_confirm,l.confirm,l.font,ink,l.glyph_size,l.label_gap,card);
 if(l.cancel_width)(void)notice_action(at+l.gap,y,l.height,l.card.action_cancel,l.cancel,l.font,ink,l.glyph_size,l.label_gap,card);
}
void gutil_connection_notices(unsigned player,bool split,int x,int y,int width,int height) {
 notice_draw(player,split,x+8,y+8,width-16,height-16,false,false);
}
void gutil_connection_notices_mini(unsigned player,bool split,int x,int y,int width,int height) {
 notice_draw(player,split,x,y,width,height,false,true);
}
void gutil_connection_notices_in_header(unsigned player,bool split,int x,int y,int width,int height) {
 notice_draw(player,split,x,y,width,height,true,false);
}

bool gutil_menu_pointer_focus(struct gutil_menu_focus* focus, bool valid,
							  float x, float y, bool navigating) {
	if(!valid) {
		focus->pointer_valid = false;
		focus->pointer_owner = false;
		if(navigating)
			focus->initialized = true;
		return false;
	}
	if(!focus->pointer_valid) {
		focus->pointer_owner = !focus->initialized;
		focus->initialized = true;
		focus->x = x;
		focus->y = y;
	}
	float dx = x - focus->x, dy = y - focus->y;
	float threshold = GUTIL_MENU_POINTER_JITTER;
	if(navigating)
		focus->pointer_owner = false;
	else if(dx * dx + dy * dy >= threshold * threshold)
		focus->pointer_owner = true;
	if(navigating || focus->pointer_owner) {
		focus->x = x;
		focus->y = y;
	}
	focus->pointer_valid = true;
	return focus->pointer_owner;
}

/* Astra ruling 2's pure activation decision, extended by ruling 5 D2 for a
 * stationary pointer after directional input. gui_util.h carries the full
 * contract. This function reads only the caller's current state; the focus
 * helper owns the movement threshold and the caller maintains origin. */
struct gutil_menu_decision
gutil_menu_activate_decide(struct gutil_menu_activate_state in) {
	struct gutil_menu_decision out = {
		.action = GUTIL_MENU_NOTHING,
		.target = GUTIL_MENU_TARGET_NONE,
		.selected = in.selected,
		.selection_origin = in.selection_origin,
	};
	if(!in.pressed)
		return out; /* no fresh edge: the caller's re-arm has not opened yet */
	if(in.navigated)
		return out; /* R-B: this frame already moved the highlight */

	/* Astra ruling 5 D2: a direction press wins over a sprite resting on
	 * another enabled target. gutil_menu_pointer_focus changes the selection
	 * origin back to POINTER only after real movement (its 4 GUI-unit
	 * threshold), so this branch remains stable through IR jitter. A disabled
	 * hovered target still consumes the press under ruling 2 item 2. */
	/* Astra D2-destructive ruling (option B, 2026-09-24): a destructive
	 * confirmation does NOT take this branch -- the mismatch falls through
	 * to rule (1)'s FOCUS_ONLY, which executes neither answer, and only a
	 * later fresh press with hovered == selected activates. */
	if(!in.destructive_dialog
	   && in.pointer_valid && in.hovered != GUTIL_MENU_TARGET_NONE
	   && in.hovered_enabled && in.hovered != in.selected
	   && in.selection_origin == GUTIL_MENU_ORIGIN_DIRECTIONAL) {
		if(in.selected != GUTIL_MENU_TARGET_NONE && in.selected_enabled) {
			out.action = GUTIL_MENU_ACTIVATE;
			out.target = in.selected;
		}
		return out;
	}

	if(in.pointer_valid && in.hovered != GUTIL_MENU_TARGET_NONE) {
		/* (1) */
		if(!in.hovered_enabled)
			return out; /* consumed; never a fallback to `selected` */
		if(in.hovered == in.selected) {
			out.action = GUTIL_MENU_ACTIVATE;
			out.target = in.hovered;
			return out;
		}
		/* ui-build-1026 T01: a select-only list ROW chooses on ONE click.
		 * Its activation is itself just a selection (Load: select + focus
		 * Play; Packs: select the pack), so no fresh-press safety is lost --
		 * Play/Apply/Delete still need their own press with the sprite on
		 * them. A destructive confirmation keeps ruling B's FOCUS_ONLY.
		 * Selection and origin are left to the caller's activate handler,
		 * like every other ACTIVATE. */
		if(in.hovered_selects_only && !in.destructive_dialog) {
			out.action = GUTIL_MENU_ACTIVATE;
			out.target = in.hovered;
			return out;
		}
		out.action = GUTIL_MENU_FOCUS_ONLY;
		out.target = in.hovered;
		out.selected = in.hovered;
		out.selection_origin = GUTIL_MENU_ORIGIN_POINTER;
		return out;
	}

	/* (2), R-A + R-E: with the sprite on NO control, only a selection the
	 * player moved to with a direction press is activatable. A POINTER-armed
	 * one is what the pointer itself chose (A6-1); a DEFAULT one is what the
	 * screen chose, and firing it on a background click discarded a
	 * sub-editor's typing (A9-2). */
	if(in.pointer_valid
	   && in.selection_origin != GUTIL_MENU_ORIGIN_DIRECTIONAL)
		return out;

	if(in.selected == GUTIL_MENU_TARGET_NONE || !in.selected_enabled)
		return out;

	/* R-D: a destructive selection needs the player to have MEANT it. The
	 * hover branch above already returned for a sprite resting on it, so
	 * reaching here with a destructive selection means the sprite is NOT on
	 * it -- and then only a direction press is intent. */
	if(in.selected_destructive
	   && in.selection_origin != GUTIL_MENU_ORIGIN_DIRECTIONAL)
		return out;

	/* (2) directional half, and (3): the highlighted target. */
	out.action = GUTIL_MENU_ACTIVATE;
	out.target = in.selected;
	return out;
}

int gutil_menu_repeat_step(struct gutil_menu_repeat* repeat, int direction,
						   float dt) {
	if(!direction) {
		*repeat = (struct gutil_menu_repeat) {0};
		return 0;
	}
	if(direction != repeat->direction) {
		repeat->direction = direction;
		repeat->remaining = 0.35F;
		return direction;
	}
	if(isfinite(dt) && dt > 0.0F)
		repeat->remaining -= dt;
	if(repeat->remaining > 0.0F)
		return 0;
	repeat->remaining = 0.1F;
	return direction;
}

/* controls.png is a 256-square, eight-column atlas of 32px cells. */
int gutil_control_icon_owned(const struct input_player_frame* input, int height,
							 int x, enum input_button b, const char* str) {
	if(!input || b < 0 || b >= IB_COUNT)
		return 0;
	if(input->join_prompt) {
		if(b != IB_GUI_CLICK)
			return 0;
		str = "Press A to join";
	} else if(input->glyphs[b] < 0)
		return 0;
	int scale = 16 * GFX_GUI_SCALE, text_scale = 5 * GFX_GUI_SCALE;
	int y = height - scale * 8 / 5;
	int width = input->join_prompt ? gutil_join_glyph(x, y, scale) :
		gutil_prompt_glyph(x, y, scale, input->glyphs[b]);
	/* A join prompt with no detected device is text only: no icon spacing. */
	int lead = width ? text_scale / 2 : 0;
	gutil_text(x + width + lead, y + (scale - text_scale) / 2, str,
			   text_scale, true);
	return width + lead + text_scale / 2 + gutil_font_width(str, text_scale);
}

/* Astra ruling 6 (push-0921 A19). See gui_util.h for the contract and why the
 * hint is text-only. The text is the MENU font at the size every other menu
 * label uses (8 units = 16 px at scale 2), centred horizontally on the screen
 * and vertically in the band, so it can never touch the list viewport above it
 * or the strip below it. */
struct gutil_button_bounds gutil_menu_nav_hint_rect(int width, int strip_top,
													const char* text) {
	int size = 8 * GFX_GUI_SCALE;
	int band = GUTIL_MENU_NAV_HINT_BAND;
	if(!text || !text[0])
		return (struct gutil_button_bounds) {0, 0, 0, 0};
	int w = gutil_font_width(text, size);
	return (struct gutil_button_bounds) {(width - w) / 2,
										 strip_top - band + (band - size) / 2,
										 w, size};
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* See gui_util.h: the draw observer (review A20-2). Test builds only. */
static unsigned nav_hint_draws;
static char nav_hint_drawn[32];
static struct gutil_button_bounds nav_hint_drawn_rect;

void gutil_menu_nav_hint_observe_reset(void) {
	nav_hint_draws = 0;
	nav_hint_drawn[0] = '\0';
	nav_hint_drawn_rect = (struct gutil_button_bounds) {0, 0, 0, 0};
}

unsigned gutil_menu_nav_hint_observed(char* text, size_t cap,
									  struct gutil_button_bounds* r) {
	if(text && cap)
		snprintf(text, cap, "%s", nav_hint_drawn);
	if(r)
		*r = nav_hint_drawn_rect;
	return nav_hint_draws;
}
#endif

void gutil_menu_nav_hint(int width, int strip_top, const char* text) {
	struct gutil_button_bounds r
		= gutil_menu_nav_hint_rect(width, strip_top, text);
	if(r.width <= 0)
		return;
	gutil_text(r.x, r.y, text, 8 * GFX_GUI_SCALE, true);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
	/* AFTER the draw, not instead of it: what this records is the text and
	 * rect that went to gutil_text on this frame. */
	nav_hint_draws++;
	snprintf(nav_hint_drawn, sizeof(nav_hint_drawn), "%s", text);
	nav_hint_drawn_rect = r;
#endif
}

int gutil_control_icon_player(unsigned player, int x, enum input_button b,
	const char* str) {
	struct input_player_frame input;
	input_prompt_frame(player, &input);
	return gutil_control_icon_owned(&input, gfx_height(), x, b, str);
}

int gutil_control_icon(int x, enum input_button b, const char* str) {
	return gutil_control_icon_player(0, x, b, str);
}

void gutil_texquad_col(int x, int y, int tx, int ty, int sx, int sy, int width,
					   int height, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	gfx_draw_quads(
		4,
		(int16_t[]) {x, y, -2, x + width, y, -2, x + width, y + height, -2, x,
					 y + height, -2},
		(uint8_t[]) {r, g, b, a, r, g, b, a, r, g, b, a, r, g, b, a},
		(uint16_t[]) {tx, ty, tx + sx, ty, tx + sx, ty + sy, tx, ty + sy});
}

void gutil_texquad(int x, int y, int tx, int ty, int sx, int sy, int width,
				   int height) {
	gutil_texquad_col(x, y, tx, ty, sx, sy, width, height, 0xFF, 0xFF, 0xFF,
					  0xFF);
}

void gutil_texquad_rt(int x, int y, int tx, int ty, int sx, int sy, int width,
					  int height) {
	gfx_draw_quads(
		4,
		(int16_t[]) {x, y, -2, x + width, y, -2, x + width, y + height, -2, x,
					 y + height, -2},
		(uint8_t[]) {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
					 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
		(uint16_t[]) {tx, ty + sy, tx, ty, tx + sx, ty, tx + sx, ty + sy});
}

void gutil_texquad_rt_any(int x, int y, float angle, int tx, int ty, int sx,
						  int sy, float width, float height) {
	width *= 0.707107F; // 1 / sqrt(2)
	height *= 0.707107F;
	angle -= glm_rad(45.0F);

	gfx_draw_quads(
		4,
		(int16_t[]) {x + sinf(angle) * width, y - cosf(angle) * height, -2,
					 x + cosf(angle) * width, y + sinf(angle) * height, -2,
					 x - sinf(angle) * width, y + cosf(angle) * height, -2,
					 x - cosf(angle) * width, y - sinf(angle) * height, -2},
		(uint8_t[]) {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
					 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
		(uint16_t[]) {tx, ty, tx + sx, ty, tx + sx, ty + sy, tx, ty + sy});
}

void gutil_bg() {
	gfx_bind_texture(&texture_terrain);

	int scale = 16 * 4;
	int cx = (gfx_width() + scale - 1) / scale;
	int cy = (gfx_height() + scale - 1) / scale;

	uint8_t tex = tex_atlas_lookup(TEXAT_DIRT);

	uint8_t s = TEX_OFFSET(TEXTURE_X(tex));
	uint8_t t = TEX_OFFSET(TEXTURE_Y(tex));

	for(int y = 0; y < cy; y++) {
		for(int x = 0; x < cx; x++) {
			gutil_texquad_col(x * scale, y * scale, s, t, 16, 16, scale, scale,
							  0x40, 0x40, 0x40, 0xFF);
		}
	}
}

static uint8_t font_char_width[256];

void gutil_reset_font(struct tex_gfx* tex) {
	assert(tex);

	int char_width = tex->width / 16;
	int char_height = tex->height / 16;

	for(int y = 0; y < 16; y++) {
		for(int x = 0; x < 16; x++) {
			int width = 0;

			for(int i = 0; i < char_width; i++) {
				bool has_pixel = false;

				for(int j = 0; j < char_height; j++) {
					uint8_t col[4];
					tex_gfx_lookup(tex, x * char_width + i, y * char_height + j,
								   col);

					if(col[0] || col[1] || col[2]) {
						has_pixel = true;
						break;
					}
				}

				if(!has_pixel)
					break;

				width++;
			}

			font_char_width[x + y * 16] = width * 8 / char_width;
		}
	}

	font_char_width[' '] = 4;
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* Headless suites have no font texture: give them metrics to measure with. */
void gutil_test_set_font_widths(const uint8_t widths[256]);
void gutil_test_set_font_widths(const uint8_t widths[256]) {
	memcpy(font_char_width, widths, 256);
}
#endif

const uint8_t* gutil_font_glyph_widths(void) {
	return font_char_width;
}

int gutil_font_width(const char* str, int scale) {
	int x = 0;

	int skip = 0;

	while(*str) {
		if(*str == '\247')
			skip = 2;

		if(skip > 0) {
			skip--;
		} else {
			x += (font_char_width[(unsigned char)*str] + 1) * scale / 8;
		}

		str++;
	}

	return x;
}

static const uint8_t chat_colors[16][3] = {
	{0x00, 0x00, 0x00}, {0x00, 0x00, 0xAA}, {0x00, 0xAA, 0x00},
	{0x00, 0xAA, 0xAA}, {0xAA, 0x00, 0x00}, {0xAA, 0x00, 0xAA},
	{0xFF, 0xAA, 0x00}, {0xAA, 0xAA, 0xAA}, {0x55, 0x55, 0x55},
	{0x55, 0x55, 0xFF}, {0x55, 0xFF, 0x55}, {0x55, 0xFF, 0xFF},
	{0xFF, 0x55, 0x55}, {0xFF, 0x55, 0xFF}, {0xFF, 0xFF, 0x55},
	{0xFF, 0xFF, 0xFF},
};

void gutil_text_rgba(int x, int y, const char* str, int scale, bool shadow,
					 uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	if(s_text_log_on && str && str[0])
		gutil_keepout(x, y, gutil_font_width(str, scale) + (shadow ? scale / 8 : 0),
					  scale + (shadow ? scale / 8 : 0));
	gfx_bind_texture(&texture_font);

	int skip = 0;
	uint8_t cr = r, cg = g, cb = b;

	while(*str) {
		if(*str == '\247')
			skip = 2;

		if(skip > 0) {
			skip--;

			int col = -1;
			if(*str >= '0' && *str <= '9')
				col = *str - '0';

			if(*str >= 'a' && *str <= 'f')
				col = *str - 'a' + 10;

			if(col >= 0) {
				cr = chat_colors[col][0];
				cg = chat_colors[col][1];
				cb = chat_colors[col][2];
			}
		} else {
			/* The atlas is byte-indexed; signed char must never index it. */
			unsigned char glyph = (unsigned char)*str;
			uint8_t tex_x = glyph % 16 * 16;
			uint8_t tex_y = glyph / 16 * 16;
			uint8_t width = (font_char_width[glyph] + 1) * scale / 8;

			if(shadow) {
				gfx_draw_quads(
					4,
					(int16_t[]) {x + scale / 8, y + scale / 8, -2,
								 x + scale + scale / 8, y + scale / 8, -2,
								 x + scale + scale / 8, y + scale + scale / 8,
								 -2, x + scale / 8, y + scale + scale / 8, -2},
					(uint8_t[]) {
						cr / 4, cg / 4, cb / 4, a, cr / 4,
						cg / 4, cb / 4, a,
						cr / 4, cg / 4, cb / 4, a, cr / 4,
						cg / 4, cb / 4, a},
					(uint16_t[]) {tex_x, tex_y, tex_x + 16, tex_y, tex_x + 16,
								  tex_y + 16, tex_x, tex_y + 16});
			}

			gfx_draw_quads(
				4,
				(int16_t[]) {x, y, -1, x + scale, y, -1, x + scale, y + scale,
							 -1, x, y + scale, -1},
				(uint8_t[]) {cr, cg, cb, a, cr, cg, cb, a,
							 cr, cg, cb, a, cr, cg, cb, a},
				(uint16_t[]) {tex_x, tex_y, tex_x + 16, tex_y, tex_x + 16,
							  tex_y + 16, tex_x, tex_y + 16});

			x += width;
		}

		str++;
	}
}

void gutil_text_rgb(int x, int y, const char* str, int scale, bool shadow,
					uint8_t r, uint8_t g, uint8_t b) {
	gutil_text_rgba(x, y, str, scale, shadow, r, g, b, 0xFF);
}

void gutil_text(int x, int y, const char* str, int scale, bool shadow) {
	// White base = chat_colors[15], the old hardcoded default.
	gutil_text_rgb(x, y, str, scale, shadow, 0xFF, 0xFF, 0xFF);
}

void gutil_text_rot(int anchor_x, int anchor_y, const char* str, int scale,
					float angle_deg, bool shadow, uint8_t r, uint8_t g,
					uint8_t b) {
	gutil_text_rot_zoom(anchor_x, anchor_y, str, scale, angle_deg, 1.0F, shadow, r, g, b);
}

void gutil_text_rot_zoom(int anchor_x, int anchor_y, const char* str, int scale,
					float angle_deg, float zoom, bool shadow, uint8_t r, uint8_t g,
					uint8_t b) {
	gfx_bind_texture(&texture_font);

	float a = glm_rad(angle_deg);
	float ca = cosf(a), sa = sinf(a);
	float ox = -(float)gutil_font_width(str, scale) / 2.0f;
	float oy = -(float)scale;
	float sh = (float)scale / 8.0f; /* the in-matrix shadow offset */

	for(; *str; str++) {
		unsigned char glyph = (unsigned char)*str;
		uint8_t tex_x = glyph % 16 * 16;
		uint8_t tex_y = glyph / 16 * 16;
		int width = (font_char_width[glyph] + 1) * scale / 8;

		for(int pass = shadow ? 0 : 1; pass < 2; pass++) {
			float px = pass == 0 ? ox + sh : ox;
			float py = pass == 0 ? oy + sh : oy;
			int16_t z = pass == 0 ? -2 : -1;
			uint8_t cr = pass == 0 ? r / 4 : r;
			uint8_t cg = pass == 0 ? g / 4 : g;
			uint8_t cb = pass == 0 ? b / 4 : b;
			float lx[4] = {px, px + scale, px + scale, px};
			float ly[4] = {py, py, py + scale, py + scale};
			int16_t v[12];
			for(int k = 0; k < 4; k++) {
				v[k * 3 + 0] = (int16_t)(anchor_x + zoom * (lx[k] * ca - ly[k] * sa));
				v[k * 3 + 1] = (int16_t)(anchor_y + zoom * (lx[k] * sa + ly[k] * ca));
				v[k * 3 + 2] = z;
			}
			gfx_draw_quads(
				4, v,
				(uint8_t[]) {cr, cg, cb, 0xFF, cr, cg, cb, 0xFF, cr, cg, cb,
							 0xFF, cr, cg, cb, 0xFF},
				(uint16_t[]) {tex_x, tex_y, tex_x + 16, tex_y, tex_x + 16,
							  tex_y + 16, tex_x, tex_y + 16});
		}

		ox += (float)width;
	}
}

void gutil_draw_item(struct item_data* item, int x, int y, int layer) {
	assert(item);

	struct item* it = item_get(item);

	if(it) {
		mat4 model;
		glm_translate_make(model, (vec3) {x, y, 0});

		gfx_depth_range(0.1F * layer, 0.1F * (layer + 1));
		gfx_write_buffers(true, true, true);
		it->renderItem(it, item, model, true, R_ITEM_ENV_INVENTORY);
		gfx_write_buffers(true, false, false);
		gfx_depth_range(0.0F, 1.0F);

		if(it->has_damage && item->durability > 0) {
			gfx_texture(false);
			gutil_texquad_col(x + 4, y + 26, 0, 0, 0, 0, 26, 4, 0, 0, 0, 255);
			gutil_texquad_col(
				x + 4, y + 26, 0, 0, 0, 0,
				26 * (1.0F - (float)item->durability / (float)it->max_damage),
				2, 4, 251, 0, 255);
			gfx_texture(true);
		}

		if(item->count > 1) {
			char count[4];
			snprintf(count, sizeof(count), "%u", item->count);
			gutil_text(17 * GFX_GUI_SCALE - gutil_font_width(count, 8 * GFX_GUI_SCALE) + x, y + 9 * GFX_GUI_SCALE, count,
					   8 * GFX_GUI_SCALE, true);
		}
	} else {
		char tmp[16];
		snprintf(tmp, sizeof(tmp), "%u", item->id);
		gutil_text(17 * GFX_GUI_SCALE - gutil_font_width(tmp, 8 * GFX_GUI_SCALE) + x, y + GFX_GUI_SCALE, tmp, 8 * GFX_GUI_SCALE,
				   true);

		snprintf(tmp, sizeof(tmp), "%u", item->count);
		gutil_text(17 * GFX_GUI_SCALE - gutil_font_width(tmp, 8 * GFX_GUI_SCALE) + x, y + 9 * GFX_GUI_SCALE, tmp, 8 * GFX_GUI_SCALE,
				   true);
	}
}

void gutil_text_field(int x, int y, int w_units, const char* text,
					  bool show_cursor) {
	int s = GFX_GUI_SCALE;
	int w = w_units * s, h = 20 * s;

	/* [740c583 GuiTextField.drawTextBox]: 0xA0A0A0 border rect, black
	 * fill, 0xE0E0E0 text, trailing '_' cursor. */
	gfx_texture(false);
	gutil_texquad_col(x - s, y - s, 0, 0, 0, 0, w + 2 * s, h + 2 * s, 0xA0,
					  0xA0, 0xA0, 255);
	gutil_texquad_col(x, y, 0, 0, 0, 0, w, h, 0, 0, 0, 255);
	gfx_texture(true);

	char shown[80];
	snprintf(shown, sizeof(shown), "%s%s", text, show_cursor ? "_" : "");
	gutil_text_rgb(x + 4 * s, y + (20 - 8) * s / 2, shown, 8 * s, true, 0xE0,
				   0xE0, 0xE0);
}

struct gutil_button_bounds gutil_button_rect(int x, int y, int w_units) {
	/* [740c583 GuiButton.GuiButton]: the default/fixed button height is 20. */
	return (struct gutil_button_bounds) {x, y, w_units * GFX_GUI_SCALE,
										  20 * GFX_GUI_SCALE};
}

bool gutil_button_hit(const struct gutil_button_bounds* bounds, float x,
					  float y) {
	assert(bounds);
	/* [740c583 GuiButton.mousePressed]: lower bounds inclusive, upper bounds
	 * exclusive. Keep this pure so synthetic pointer tests need no GL context. */
	return x >= bounds->x && x < bounds->x + bounds->width && y >= bounds->y
		&& y < bounds->y + bounds->height;
}

bool gutil_button_even_width(int w_units) {
	/* [740c583 GuiButton.drawButton] always splits into two halves via
	 * integer width/2 -- gutil_button_draw_bg below replicates that exactly.
	 * The split is lossless only when w_units is even (half*2 == w_units);
	 * an odd w_units silently drops a 1-unit seam that the old per-screen
	 * private draws (lw=bw/2, rw=bw-lw, always exact) would NOT have
	 * dropped -- recorded as debt in 7a10eb4's commit body. Every button on
	 * this helper today is even (200/150/98/70); kept pure and exported so
	 * the invariant is unit-testable without a GL context
	 * (test_slotlist_env.c), and gutil_button_draw_bg asserts it at the
	 * split site so a future odd-width button fails loudly instead of
	 * silently diverging. */
	return w_units % 2 == 0;
}

static void gutil_button_draw_bg(int x, int y, int w_units, bool enabled,
								 bool hover) {
	int s = GFX_GUI_SCALE;
	/* [740c583 GuiButton.getHoverState/drawButton]: strip rows are
	 * disabled=46, idle=66, hover=86; every button is 20 units tall. */
	int ty = enabled ? (hover ? 86 : 66) : 46;
	int half = w_units / 2;
	/* Odd-width guard (the split site): an odd w_units would silently drop
	 * a 1-unit seam here instead of matching the old private per-screen
	 * math -- fail loudly rather than ship the drift. */
	assert(gutil_button_even_width(w_units)
		   && "gutil_button_draw_bg: w_units must be even -- odd widths drop "
			  "a 1-unit half-split seam (see gutil_button_even_width)");
	gfx_bind_texture(&texture_gui_widgets);
	/* [740c583 GuiButton.drawButton]: both halves use integer width/2. */
	gutil_texquad(x, y, 0, ty, half, 20, half * s, 20 * s);
	gutil_texquad(x + half * s, y, 200 - half, ty, half, 20, half * s,
				  20 * s);
}

static void gutil_button_draw_label(int x, int y, int w_units,
									const char* label, bool enabled,
									bool hover) {
	assert(label);
	int s = GFX_GUI_SCALE;
	/* [740c583 GuiButton.drawButton]: 0xA0A0A0 disabled, 0xE0E0E0 idle,
	 * and 0xFFFFE0 hover. */
	uint8_t r = 0xE0, g = 0xE0, b = 0xE0;
	if(!enabled)
		r = g = b = 0xA0;
	else if(hover)
		r = g = 0xFF, b = 0xE0;
	int tw = gutil_font_width(label, 8 * s);
	gutil_text_rgb(x + (w_units * s - tw) / 2, y + (20 - 8) * s / 2,
				   label, 8 * s, true, r, g, b);
}

void gutil_button_draw(int x, int y, int w_units, const char* label,
					   bool enabled, bool hover) {
	assert(label);
	/* Pure composition of the two pieces above -- byte-identical to the
	 * pre-refactor monolithic body (same calls, same arguments, same order);
	 * factored so gutil_button_draw_dwell (below) can insert its overlay
	 * between them. */
	gutil_button_draw_bg(x, y, w_units, enabled, hover);
	gutil_button_draw_label(x, y, w_units, label, enabled, hover);
}

int gutil_button_dwell_fill_px(int w_units, float fill_frac) {
	/* Wii-controller hold-to-confirm carve-out #2
	 * (docs/wii_controller_playability.md; Beta's GuiButton has no dwell
	 * state) -- the charged width in PHYSICAL pixels at fill_frac (clamped
	 * to [0,1]) of a w_units-wide button. GFX_GUI_SCALE is applied once at
	 * this logical-to-physical boundary, like every other site in this
	 * file. Pure and exported so the fill math is unit-testable without a
	 * GL context. */
	float p = fill_frac;
	if(p < 0.0f)
		p = 0.0f;
	if(p > 1.0f)
		p = 1.0f;
	return (int)((float)(w_units * GFX_GUI_SCALE) * p);
}

void gutil_button_draw_dwell(int x, int y, int w_units, const char* label,
							 bool enabled, bool hover, float fill_frac) {
	gutil_button_draw_bg(x, y, w_units, enabled, hover);

	/* fill_frac <= 0 -- including exactly 0 -- draws NO overlay, matching
	 * the death screen's original `hold_s > 0.0f` gate: an at-rest button
	 * shows the plain hover row, not a fully-dimmed one. The dimmed rect
	 * covers the UNFILLED remainder [x+fill, x+w_units*s) so the undimmed
	 * portion reads as charge filling left-to-right; black, alpha 110,
	 * untextured, drawn OVER the bg and UNDER the label so the label stays
	 * legible on top -- the exact order and values screen_death.c's private
	 * draw used before this helper existed. */
	if(fill_frac > 0.0f) {
		int s = GFX_GUI_SCALE;
		int fill = gutil_button_dwell_fill_px(w_units, fill_frac);
		gfx_texture(false);
		gutil_texquad_col(x + fill, y, 0, 0, 0, 0, w_units * s - fill,
						  20 * s, 0, 0, 0, 110);
		gfx_texture(true);
	}

	gutil_button_draw_label(x, y, w_units, label, enabled, hover);
}

/* Small equipment/crafting slots retain the actual stack and wear overlays. */
void gutil_draw_item_sized(struct item_data* item,int x,int y,int size,int layer) {
 if(size==32){gutil_draw_item(item,x,y,layer);return;}
 assert(item && size>0);
 struct item* it=item_get(item);
 if(it){
  mat4 model;glm_translate_make(model,(vec3){x,y,0});
  float q=size/32.0F;glm_scale(model,(vec3){q,q,1});
  gfx_depth_range(.1F*layer,.1F*(layer+1));gfx_write_buffers(true,true,true);
  it->renderItem(it,item,model,true,R_ITEM_ENV_INVENTORY);
  gfx_write_buffers(true,false,false);gfx_depth_range(0,1);
  if(it->has_damage && it->max_damage && item->durability>0){
   gfx_texture(false);
   gutil_texquad_col(x+4*q,y+26*q,0,0,0,0,26*q,4*q,0,0,0,255);
   gutil_texquad_col(x+4*q,y+26*q,0,0,0,0,26*q*(1.0F-(float)item->durability/it->max_damage),2*q,4,251,0,255);
   gfx_texture(true);
  }
 }else{
  char id[16];snprintf(id,sizeof id,"%u",item->id);
  gutil_text(x+size-gutil_font_width(id,12),y,id,12,true);
 }
 if(item->count>1){char count[4];snprintf(count,sizeof count,"%u",item->count);
  gutil_text(x+size-gutil_font_width(count,16),y+size-14,count,16,true);}
}

#define RECOVERY_TEXT 16
#define RECOVERY_ROW 20
#define RECOVERY_GLYPH 24
static int recovery_glyph_width(int glyph) {
 return gutil_prompt_glyph_width(glyph,RECOVERY_GLYPH);
}
void recovery_card_layout(int x,int y,int width,int height,bool split,
 const struct controller_recovery_card* c,bool resume,int glyph,
 struct recovery_card_layout* out) {
 memset(out,0,sizeof *out);
 if(!c)return;
 int pad=split?10:14,gap=split?8:10,pic_h=split?48:64;
 const char* rows[3]={c->title,c->detail,c->status};
 out->rows=c->status[0]?3:2;
 int tw=0;
 for(int k=0;k<out->rows;k++){int w=gutil_font_width(rows[k],RECOVERY_TEXT);if(w>tw)tw=w;}
 int pic_w=c->picture==CONTROLLER_PICTURE_NONE?0:gutil_controller_picture_width(c->picture,pic_h);
 int body=out->rows*RECOVERY_ROW;if(pic_w&&pic_h>body)body=pic_h;
 int w=pad*2+(pic_w?pic_w+16:0)+tw;
 out->glyph_w=resume?recovery_glyph_width(glyph):0;
 int bw=12+out->glyph_w+(out->glyph_w?6:0)+gutil_font_width("Resume",RECOVERY_TEXT)+12;
 if(resume&&bw+2*pad>w)w=bw+2*pad;
 int h=pad*2+body+(resume?gap+32:0);
 out->card=(struct gutil_button_bounds){x+(width-w)/2,y+(height-h)/2,w,h};
 int cy=out->card.y+pad+body/2;
 out->picture=(struct gutil_button_bounds){out->card.x+pad,cy-pic_h/2,pic_w,pic_w?pic_h:0};
 out->text_x=out->card.x+pad+(pic_w?pic_w+16:0);
 /* each row's 7-row letters (14 px at 16) centred on its own line centre */
 int first=cy-out->rows*RECOVERY_ROW/2+RECOVERY_ROW/2;
 for(int k=0;k<out->rows;k++)out->row_y[k]=first+k*RECOVERY_ROW-7;
 out->has_button=resume;
 if(resume) {
  out->button=(struct gutil_button_bounds){out->card.x+(w-bw)/2,out->card.y+pad+body+gap,bw,32};
  out->glyph_x=out->button.x+12;out->glyph_y=out->button.y+4;
  out->label_x=out->glyph_x+out->glyph_w+(out->glyph_w?6:0);out->label_y=out->button.y+16-7;
 }
}
void recovery_card_draw(const struct recovery_card_layout* l,
 const struct controller_recovery_card* c,int glyph,bool focus) {
 if(!l||!c||l->card.width<=0)return;
 const struct gutil_button_bounds* r=&l->card;
 gfx_texture(false);
 gutil_texquad_col(r->x-2,r->y-2,0,0,0,0,r->width+4,r->height+4,17,17,17,255);
 gutil_texquad_col(r->x,r->y,0,0,0,0,r->width,r->height,198,198,198,255);
 gutil_texquad_col(r->x,r->y,0,0,0,0,r->width,2,255,255,255,153);
 gutil_texquad_col(r->x,r->y,0,0,0,0,2,r->height,255,255,255,153);
 gutil_texquad_col(r->x,r->y+r->height-2,0,0,0,0,r->width,2,85,85,85,255);
 gutil_texquad_col(r->x+r->width-2,r->y,0,0,0,0,2,r->height,85,85,85,255);
 if(l->has_button) {
  const struct gutil_button_bounds* b=&l->button;
  if(focus)gutil_texquad_col(b->x-2,b->y-2,0,0,0,0,b->width+4,b->height+4,255,255,255,255);
  gutil_texquad_col(b->x,b->y,0,0,0,0,b->width,b->height,120,120,120,255);
 }
 gfx_texture(true);
 if(l->picture.width)gutil_controller_picture(l->picture.x,l->picture.y,l->picture.width,l->picture.height,c->picture);
 const char* rows[3]={c->title,c->detail,c->status};
 for(int k=0;k<l->rows;k++) {
  uint8_t ink=k<2?40:70;
  gutil_text_rgb(l->text_x,l->row_y[k],rows[k],RECOVERY_TEXT,false,ink,ink,ink);
 }
 if(l->has_button) {
  if(l->glyph_w)(void)gutil_prompt_glyph(l->glyph_x,l->glyph_y,RECOVERY_GLYPH,glyph);
  gutil_text(l->label_x,l->label_y,"Resume",RECOVERY_TEXT,true);
 }
}
