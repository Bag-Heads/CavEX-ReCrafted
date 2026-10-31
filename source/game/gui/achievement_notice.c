/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../../platform/gfx.h"
#include "../../graphics/gui_util.h"
#include "../../platform/input_glyph.h"
#include "../../platform/menu_sound.h"
#include "../achievements/achievements_api.h"
#include "achievement_notice.h"
#include "screen_achievements.h"

struct notice_view {
	uint64_t generation;
	char profile[CW_PROFILE_ID_MAX + 1];
	int front;
	unsigned elapsed, parked;
	ptime_t segment;
	bool visible, was_visible, rang;
};
static struct notice_view views[CW_ACH_SLOT_COUNT];
static ptime_t frame_now;
static struct tex_gfx paper;
static uint8_t paper_source[256];

void achievement_notice_init(void) {
	memset(views, 0, sizeof views);
	for(unsigned i = 0; i < CW_ACH_SLOT_COUNT; i++)
		views[i].front = -1;
}
void achievement_notice_shutdown(void) {
	tex_gfx_clear(&paper);
	achievement_notice_init();
}
void achievement_notice_update(ptime_t now) {
	frame_now = now;
	for(unsigned i = 0; i < CW_ACH_SLOT_COUNT; i++) {
		struct notice_view* v = &views[i];
		struct ach_status st;
		bool bound = achievements_status(i, &st) && st.bound && !st.closing;
		if(!bound || v->generation != st.generation
		   || strcmp(v->profile, st.profile_id)) {
			*v = (struct notice_view) {.front = -1};
			if(bound) {
				v->generation = st.generation;
				memcpy(v->profile, st.profile_id, sizeof v->profile);
			}
		}
		v->was_visible = v->visible;
		v->visible = false;
	}
}

/* The fixed approved names are at most two lines. Wrap whole words using
 * the active pack's real native font metrics, also used by the board. */
static void name_lines(const char* name, int width, char lines[2][64]) {
	memset(lines, 0, 128);
	if(gutil_font_width(name, 16) <= width) {
		strncpy(lines[0], name, 63);
		return;
	}
	const char* cut = NULL;
	for(const char* p = name; *p; p++) {
		if(*p != ' ')
			continue;
		size_t len = (size_t)(p - name);
		if(len >= 64)
			break;
		memcpy(lines[0], name, len);
		lines[0][len] = 0;
		if(gutil_font_width(lines[0], 16) > width)
			break;
		cut = p;
	}
	if(cut) {
		size_t len = (size_t)(cut - name);
		memcpy(lines[0], name, len);
		lines[0][len] = 0;
		strncpy(lines[1], cut + 1, 63);
	}
}

void achievement_notice_layout(struct achievement_notice_frame* f, int x, int y,
							   int w, int h, int ach_id, bool hint,
							   const struct input_player_frame* input) {
	bool split = h < 480;
	*f = (struct achievement_notice_frame) {
		.ach_id = ach_id,
		.glyph = hint && input ? input->glyphs[IB_OPEN_INVENTORY] : -1,
		.hint = hint};
	f->card = (struct menu_rect) {x + w - (split ? 32 + 280 : 24 + 320),
								  y + (split ? 12 : 24), split ? 280 : 320,
								  split ? 56 : 64};
	int inset = split ? 8 : 10, size = split ? 40 : 44;
	f->slot
		= (struct menu_rect) {f->card.x + inset, f->card.y + inset, size, size};
	int tx = f->card.x + (split ? 56 : 64), tw = split ? 216 : 244;
	f->caption = (struct menu_rect) {
		tx, f->card.y + (hint ? (split ? 4 : 8) : (split ? 8 : 12)), tw, 20};
	f->name = (struct menu_rect) {tx, f->card.y + (split ? 28 : 32), tw,
								  hint ? 24 : 20};
	int glyph_width = gutil_prompt_glyph_width(f->glyph, 24);
	if(!glyph_width)
		f->glyph = -1;
	char lines[2][64];
	name_lines(hint ? "Open inventory" : screen_achievements_name(ach_id),
			   tw - (glyph_width ? glyph_width + 8 : 0), lines);
	f->wrapped = lines[1][0] != 0;
	if(f->wrapped) {
		f->card.h += 20;
		f->name.h += 20;
	}
}

static bool safe(struct menu_rect card, int x, int y, int w, int h,
				 const struct menu_rect* occupied, size_t count) {
	if(card.x < x || card.x + card.w > x + w || card.y < y
	   || card.y + card.h > y + h)
		return false;
	/* Include the slide path from the viewport's top, not just the hold rect.
	 */
	card.h += card.y - y;
	card.y = y;
	for(size_t i = 0; i < count; i++) {
		struct menu_rect r = occupied[i];
		if(r.w > 0 && r.h > 0 && card.x < r.x + r.w + 8
		   && card.x + card.w + 8 > r.x && card.y < r.y + r.h + 8
		   && card.y + card.h + 8 > r.y)
			return false;
	}
	return true;
}

/* Approved prototype's smoked paper: crop the existing crafting paper,
 * clamp its red-channel grain to +/-4, repeat at 2x. This is a derived
 * material, not new artwork. A changed crop replaces it on the main thread
 * after the application's render-completed handoff, like other UI textures. */
static bool paper_prepare(void) {
	uint8_t source[256];
	for(int y = 0; y < 16; y++)
		for(int x = 0; x < 16; x++) {
			uint8_t c[4] = {198, 198, 198, 255};
			if(texture_gui_crafting.data && texture_gui_crafting.width >= 166
			   && texture_gui_crafting.height >= 78)
				tex_gfx_lookup(&texture_gui_crafting, 150 + x, 62 + y, c);
			source[y * 16 + x] = c[0];
		}
	if(paper.data && !memcmp(source, paper_source, sizeof source))
		return true;
	uint8_t* pixels = malloc(32 * 32 * 4);
	if(!pixels)
		return false;
	for(int y = 0; y < 32; y++)
		for(int x = 0; x < 32; x++) {
			int d = (int)floorf((source[(y / 2) * 16 + x / 2] - 198) / 4.0F
								+ .5F);
			if(d < -4)
				d = -4;
			if(d > 4)
				d = 4;
			unsigned i = (y * 32 + x) * 4;
			pixels[i] = 100 + d;
			pixels[i + 1] = 96 + d;
			pixels[i + 2] = 87 + d;
			pixels[i + 3] = 255;
		}
	tex_gfx_clear(&paper);
	if(!tex_gfx_load(&paper, pixels, 32, 32, TEX_FMT_RGBA32, false))
		return false; /* tex_gfx_load consumes pixels on either path */
	memcpy(paper_source, source, sizeof source);
	return true;
}
static void solid(struct menu_rect r, int dy, uint8_t red, uint8_t green,
				  uint8_t blue) {
	gfx_texture(false);
	gutil_texquad_col(r.x, r.y + dy, 0, 0, 0, 0, r.w, r.h, red, green, blue,
					  255);
	gfx_texture(true);
}
static void paint(const struct achievement_notice_frame* f) {
	struct menu_rect r = f->card;
	int dy = f->slide_y;
	gfx_bind_texture(&paper);
	for(int y = 0; y < r.h; y += 32)
		for(int x = 0; x < r.w; x += 32) {
			int w = r.w - x < 32 ? r.w - x : 32;
			int h = r.h - y < 32 ? r.h - y : 32;
			gutil_texquad(r.x + x, r.y + y + dy, 0, 0, w, h, w, h);
		}
	solid((struct menu_rect) {r.x, r.y, r.w, 2}, dy, 37, 37, 37);
	solid((struct menu_rect) {r.x, r.y + r.h - 2, r.w, 2}, dy, 37, 37, 37);
	solid((struct menu_rect) {r.x, r.y, 2, r.h}, dy, 37, 37, 37);
	solid((struct menu_rect) {r.x + r.w - 2, r.y, 2, r.h}, dy, 37, 37, 37);
	solid((struct menu_rect) {r.x + 2, r.y + 2, r.w - 4, 2}, dy, 226, 222, 211);
	solid((struct menu_rect) {r.x + 2, r.y + 2, 2, r.h - 4}, dy, 226, 222, 211);
	solid((struct menu_rect) {r.x + 2, r.y + r.h - 4, r.w - 4, 2}, dy, 69, 65,
		  59);
	solid((struct menu_rect) {r.x + r.w - 4, r.y + 2, 2, r.h - 4}, dy, 69, 65,
		  59);
	gfx_bind_texture(&texture_gui_crafting);
	gutil_texquad(f->slot.x, f->slot.y + dy, 29, 16, 18, 18, f->slot.w,
				  f->slot.h);
}

bool achievement_notice_render(unsigned owner,
							   const struct input_player_frame* input, int x,
							   int y, int w, int h, bool blocked,
							   const struct menu_rect* occupied,
							   size_t occupied_count,
							   struct achievement_notice_frame* out) {
	if(out)
		*out = (struct achievement_notice_frame) {.ach_id = -1, .glyph = -1};
	if(owner >= CW_ACH_SLOT_COUNT)
		return false;
	struct notice_view* v = &views[owner];
	struct ach_notice n;
	bool toast = achievements_notice_peek(owner, &n);
	if(blocked || (!toast && !n.hint_active))
		return false;
	if(!toast || v->front != n.ach_id) {
		v->front = toast ? n.ach_id : -1;
		v->elapsed = v->parked = 0;
		v->was_visible = v->rang = false;
	}
	struct achievement_notice_frame f;
	achievement_notice_layout(&f, x, y, w, h, toast ? n.ach_id : 0, !toast,
							  input);
	if(!safe(f.card, x, y, w, h, occupied, occupied_count) || !paper_prepare())
		return false;
	if(toast) {
		if(!v->was_visible) {
			v->parked = v->elapsed;
			v->segment = frame_now;
		}
		int dt = time_diff_ms(v->segment, frame_now);
		v->elapsed = v->parked + (dt > 0 ? (unsigned)dt : 0);
		if(v->elapsed >= ACHIEVEMENT_NOTICE_WINDOW_MS) {
			achievements_notice_ack(owner);
			v->front = -1;
			v->was_visible = false;
			return achievement_notice_render(owner, input, x, y, w, h, blocked,
											 occupied, occupied_count, out);
		}
		f.elapsed_ms = v->elapsed;
		float edge = v->elapsed < ACHIEVEMENT_NOTICE_SLIDE_MS ?
			1.0F - v->elapsed / (float)ACHIEVEMENT_NOTICE_SLIDE_MS :
			v->elapsed
				> ACHIEVEMENT_NOTICE_WINDOW_MS - ACHIEVEMENT_NOTICE_SLIDE_MS ?
			(v->elapsed
			 - (ACHIEVEMENT_NOTICE_WINDOW_MS - ACHIEVEMENT_NOTICE_SLIDE_MS))
				/ (float)ACHIEVEMENT_NOTICE_SLIDE_MS :
			0.0F;
		f.slide_y = -(int)roundf(edge * edge * edge * edge * (f.card.h + 30));
	}
	paint(&f);
	struct ach_desc row;
	if(achievements_get(owner, toast ? n.ach_id : 0, &row)) {
		struct item_data item = {.id = row.icon_item, .count = 1};
		gutil_draw_item_sized(&item, f.slot.x + (f.slot.w - 32) / 2,
							  f.slot.y + f.slide_y + (f.slot.h - 32) / 2, 32,
							  0);
	}
	gutil_text_rgb(f.caption.x, f.caption.y + f.slide_y,
				   toast ? "Achievement get!" : "Taking Inventory", 16, false,
				   255, 240, 176);
	int tx = f.name.x;
	if(f.hint && f.glyph >= 0) {
		tx += gutil_prompt_glyph(tx, f.name.y + f.slide_y, 24, f.glyph) + 8;
	}
	char lines[2][64];
	name_lines(f.hint ? "Open inventory" : screen_achievements_name(f.ach_id),
			   f.name.w - (tx - f.name.x), lines);
	for(int i = 0; i < 2 && lines[i][0]; i++)
		gutil_text_rgb(tx, f.name.y + f.slide_y + (f.hint ? 2 : 0) + i * 20,
					   lines[i], 16, false, 255, 248, 231);
	v->visible = true;
	if(toast && !v->rang && f.card.y + f.slide_y + f.card.h > y) {
		v->rang = true;
		audio_achievement_bell();
	}
	if(out)
		*out = f;
	return true;
}
