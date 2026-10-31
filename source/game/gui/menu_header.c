/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include "menu_header.h"

/* See menu_header.h for the design authority and the composition rule this
 * file implements. Pure geometry: nothing here touches gfx, audio or gstate,
 * so both the shipping screens and the golden drive the SAME code. */

enum menu_screen_layout menu_screen_layout_for(int width, int height) {
	(void)width;
	return height >= 480 ? MENU_SCREEN_FULL : MENU_SCREEN_COMPACT;
}

bool menu_rect_empty(const struct menu_rect* r) {
	return !r || r->w <= 0 || r->h <= 0;
}

bool menu_rect_overlap(const struct menu_rect* a, const struct menu_rect* b) {
	if(menu_rect_empty(a) || menu_rect_empty(b))
		return false;
	return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h
		&& b->y < a->y + a->h;
}

bool menu_rect_contains(const struct menu_rect* outer,
						const struct menu_rect* inner) {
	if(menu_rect_empty(outer) || menu_rect_empty(inner))
		return false;
	return inner->x >= outer->x && inner->y >= outer->y
		&& inner->x + inner->w <= outer->x + outer->w
		&& inner->y + inner->h <= outer->y + outer->h;
}

bool menu_split_composition_admitted(bool second_present, bool second_ready) {
	return second_present && second_ready;
}

struct menu_rect menu_notice_band(int view_x, int view_y, int view_w) {
	struct menu_rect band = {view_x + MENU_NOTICE_BAND_INSET,
							 view_y + MENU_NOTICE_BAND_TOP,
							 view_w - 2 * MENU_NOTICE_BAND_INSET,
							 MENU_NOTICE_BAND_H};
	if(band.w < 0)
		band.w = 0;
	return band;
}

void menu_composition_begin(struct menu_composition* c, enum menu_screen_id menu,
							int view_x, int view_y, int view_w, int view_h) {
	if(!c)
		return;
	struct menu_composition blank = {0};
	*c = blank;
	c->menu = menu;
	c->layout = menu_screen_layout_for(view_w, view_h);
	c->view = (struct menu_rect) {view_x, view_y, view_w, view_h};
	c->window = c->view;
}

void menu_composition_window(struct menu_composition* c, int x, int y, int w,
							 int h) {
	if(!c)
		return;
	c->window = (struct menu_rect) {x, y, w, h};
}

void menu_composition_header(struct menu_composition* c, int x, int y, int w,
							 int h) {
	if(!c)
		return;
	c->header = (struct menu_rect) {x, y, w, h};
	c->has_header = w > 0 && h > 0;
}

void menu_composition_header_notice(struct menu_composition* c,int desired,int title) {
 if(!c||!c->has_header)return;
 if(desired<0)desired=0;
 if(title<0)title=0;
 int gap=desired?8:0,max=c->header.w-title-gap;if(max<0)max=0;
 c->notice_reserve_w=desired<max?desired:max;c->notice_gap=c->notice_reserve_w?gap:0;
}
struct menu_rect
menu_composition_header_content(const struct menu_composition* c) {
	struct menu_rect empty = {0, 0, 0, 0};
	if(!c || !c->has_header)
		return empty;
	struct menu_rect content = c->header;
	content.w -= c->notice_reserve_w+c->notice_gap;
	if(content.w < 0)
		content.w = 0;
	return content;
}

void menu_composition_action(struct menu_composition* c, int x, int y, int w,
							 int h) {
	if(!c)
		return;
	if(w <= 0 || h <= 0)
		return;
	if(c->action_count >= MENU_ACTION_MAX) {
		c->action_overflow++;
		return;
	}
	c->actions[c->action_count++] = (struct menu_rect) {x, y, w, h};
}

void menu_composition_seal(struct menu_composition* c) {
	if(!c)
		return;
	/* The approved rule, verbatim: header composition only in the compact
	 * (split) layout, and only for a menu that owns a header. The death screen
	 * declares none, so it always keeps the floating card. */
	c->notice_in_header = c->has_header && c->layout == MENU_SCREEN_COMPACT;
	if(c->notice_in_header) {
		c->notice=(struct menu_rect){c->header.x+c->header.w-c->notice_reserve_w,c->header.y,c->notice_reserve_w,c->header.h};
		return;
	}
	c->notice = menu_notice_band(c->view.x, c->view.y, c->view.w);
}

size_t menu_composition_notice_conflict(const struct menu_composition* c) {
	if(!c)
		return 0;
	for(size_t k = 0; k < c->action_count; k++)
		if(menu_rect_overlap(&c->notice, &c->actions[k]))
			return k;
	return c->action_count;
}

/* ---- pause -------------------------------------------------------------- */

/* Approved menu prototype geometry, shared by rendering and hit testing. */
struct menu_rect menu_pause_row(const struct menu_composition* c, int index) {
 struct menu_rect r = {0};
 if(!c || index < 0) return r;
 bool full = c->layout == MENU_SCREEN_FULL;
 r.w = full ? 320 : 288;
 r.h = full ? 44 : 32;
 r.x = c->view.x + (c->view.w-r.w)/2;
 r.y = c->view.y + (full ? 104 : 48) + index*(r.h+(full ? 2 : 4));
 return r;
}
struct menu_rect menu_pause_status(const struct menu_composition* c, int rows) {
 struct menu_rect r = {0};
 if(!c || rows <= 0) return r;
 struct menu_rect last = menu_pause_row(c, rows-1);
 r.x=c->view.x; r.w=c->view.w; r.h=c->layout==MENU_SCREEN_FULL ? 64 : 20; /* split: 20 px text-only */
 r.y=last.y+last.h+(c->layout==MENU_SCREEN_FULL ? 16 : 8);
 return r;
}

/* ---- compact container header ------------------------------------------ */

/* Fixed boxes inside the compact 640x240 content box. Deliberately not
 * derived from measured tab text: a font-dependent centre could drift into
 * the Next prompt or the notice slot on one pack and not the other, and the
 * golden could not prove otherwise without a GL context. The tab row is
 * centred INSIDE menu_container_tab_box and clipped to it. */
struct menu_rect menu_container_header_box(int wx, int wy) {
	struct menu_rect r = {wx + 8, wy + MENU_CONTAINER_HEADER_Y, 624,
						  MENU_CONTAINER_HEADER_H};
	return r;
}

/* Prompt boxes are the REAL hint() footprint -- a 32 px glyph plus its label,
 * 136x32 -- not just the glyph, so the golden proves the drawn control clears
 * the notice slot, not an optimistic sub-rectangle of it. */
struct menu_rect menu_container_prev_box(int wx, int wy) {
	struct menu_rect r = {wx + 24, wy + MENU_CONTAINER_HEADER_Y, 136,
						  MENU_CONTAINER_HEADER_H};
	return r;
}

struct menu_rect menu_container_tab_box(int wx, int wy) {
	struct menu_rect r = {wx + 168, wy + 16, 204, 22};
	return r;
}

struct menu_rect menu_container_next_box(int wx, int wy) {
	/* Ends exactly at the reserved notice slot -- the header box spans
	 * [wx + 8, wx + 632), so the slot is [wx + 512, wx + 632). The
	 * pre-approval position (wx + 524) sat INSIDE it, which is the collision
	 * this reserve removes. */
	struct menu_rect r = {wx + 376, wy + MENU_CONTAINER_HEADER_Y, 136,
						  MENU_CONTAINER_HEADER_H};
	return r;
}

/* ---- sign editor -------------------------------------------------------- */

struct menu_rect menu_sign_count_box(const struct menu_composition* c) {
	struct menu_rect r = {0, 0, 0, 0};
	if(!c || !c->has_header)
		return r;
	struct menu_rect content = menu_composition_header_content(c);
	if(content.w < MENU_SIGN_COUNT_RESERVE_W)
		return r;
	r.x = content.x + content.w - MENU_SIGN_COUNT_RESERVE_W;
	r.y = content.y + 2;
	r.w = MENU_SIGN_COUNT_RESERVE_W;
	r.h = content.h - 4;
	if(r.h < 0)
		r.h = 0;
	return r;
}

/* ---- death -------------------------------------------------------------- */

struct menu_rect menu_death_row(const struct menu_composition* c,int index) {
 if(!c||index<0||index>1)return (struct menu_rect){0};
 if(c->layout==MENU_SCREEN_FULL)return (struct menu_rect){c->view.x+(c->view.w-340)/2,c->view.y+180+index*50,340,40};
 return (struct menu_rect){c->view.x+(c->view.w-492)/2+index*252,c->view.y+120,240,36};
}
