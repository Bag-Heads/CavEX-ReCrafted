/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "widget_slotlist.h"

#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../platform/gfx.h"

#include <assert.h>
#include <limits.h>

static int slotlist_content_height(const struct slotlist* list) {
	if(list->count > (size_t)INT_MAX / (size_t)list->row_height)
		return INT_MAX;
	return (int)list->count * list->row_height;
}

static void slotlist_clamp(struct slotlist* list) {
	/* [740c583 GuiSlot.getContentHeight/bindAmountScrolled]: subtract the
	 * viewport minus 4 units; a negative maximum is halved before the two
	 * ordered clamps, which intentionally leaves a negative centered offset. */
	int max = slotlist_content_height(list)
		- (list->bottom - list->top - 4 * GFX_GUI_SCALE);
	if(max < 0)
		max /= 2;
	if(list->amount_scrolled < 0)
		list->amount_scrolled = 0;
	if(list->amount_scrolled > max)
		list->amount_scrolled = max;
}

void slotlist_reset(struct slotlist* list, int width, int top, int bottom,
					int row_height, size_t count, size_t selection) {
	assert(list);
	assert(width > 0 && bottom >= top && row_height > 0);
	*list = (struct slotlist) {
		.width = width,
		.top = top,
		.bottom = bottom,
		.row_height = row_height,
		.count = count,
		.selection = selection < count ? selection : SLOT_LIST_NO_SELECTION,
		.last_clicked = -1,
	};
	slotlist_clamp(list);
}

void slotlist_set_count(struct slotlist* list, size_t count) {
	assert(list);
	list->count = count;
	if(count == 0)
		list->selection = SLOT_LIST_NO_SELECTION;
	else if(list->selection >= count)
		list->selection = count - 1;
	if(list->last_clicked >= 0 && (size_t)list->last_clicked >= count) {
		list->last_clicked = -1;
		list->has_last_click = false;
	}
	slotlist_clamp(list);
}

void slotlist_select(struct slotlist* list, size_t selection, bool reveal) {
	assert(list);
	if(selection >= list->count) {
		list->selection = SLOT_LIST_NO_SELECTION;
		return;
	}
	list->selection = selection;
	if(reveal) {
		int inset = 4 * GFX_GUI_SCALE;
		int y = list->top + inset - list->amount_scrolled
			+ (int)selection * list->row_height;
		if(y < list->top + inset)
			list->amount_scrolled = (int)selection * list->row_height;
		else if(y + list->row_height > list->bottom)
			list->amount_scrolled = (int)(selection + 1) * list->row_height
				- (list->bottom - list->top - inset);
		slotlist_clamp(list);
	}
}

size_t slotlist_selected(const struct slotlist* list) {
	assert(list);
	return list->selection;
}

int slotlist_amount_scrolled(const struct slotlist* list) {
	assert(list);
	return list->amount_scrolled;
}

void slotlist_scroll(struct slotlist* list, int direction) {
	assert(list);
	/* [740c583 GuiSlot.actionPerformed]: posZ * 2 / 3 per scroll button. */
	if(direction < 0)
		list->amount_scrolled -= list->row_height * 2 / 3;
	else if(direction > 0)
		list->amount_scrolled += list->row_height * 2 / 3;
	slotlist_clamp(list);
}

void slotlist_pointer_scroll(struct slotlist* list, float y, bool held) {
	assert(list);
	/* [740c583 GuiSlot.drawScreen]: a held pointer drags amountScrolled in
	 * the opposite direction at 1:1 speed while it remains in the viewport. */
	if(!held || y < list->top || y > list->bottom) {
		list->pointer_scrolling = false;
		return;
	}
	if(!list->pointer_scrolling) {
		list->pointer_y = y;
		list->pointer_scrolling = true;
		return;
	}
	list->amount_scrolled -= (int)(y - list->pointer_y);
	list->pointer_y = y;
	slotlist_clamp(list);
}

int slotlist_index_at(const struct slotlist* list, float x, float y) {
	assert(list);
	/* [740c583 GuiSlot.func_27256_c/drawScreen]: width/2 +/- 110 and the
	 * viewport edges are inclusive; row content starts four units below top. */
	int left = list->width / 2 - 110 * GFX_GUI_SCALE;
	int right = list->width / 2 + 110 * GFX_GUI_SCALE;
	if(x < left || x > right || y < list->top || y > list->bottom)
		return -1;
	int relative = (int)y - list->top + list->amount_scrolled
		- 4 * GFX_GUI_SCALE;
	if(relative < 0)
		return -1;
	int index = relative / list->row_height;
	return index >= 0 && (size_t)index < list->count ? index : -1;
}

enum slotlist_click_result slotlist_click_at(struct slotlist* list, float x,
										 float y, int64_t now_ms) {
	assert(list);
	int index = slotlist_index_at(list, x, y);
	if(index < 0)
		return SLOT_LIST_CLICK_NONE;

	/* [740c583 GuiSlot.drawScreen]: activate only when the SAME selected row
	 * is clicked again strictly within 250 ms. */
	bool double_click = list->selection == (size_t)index
		&& list->last_clicked == index && list->has_last_click
		&& now_ms >= list->last_click_ms && now_ms - list->last_click_ms < 250;
	list->selection = (size_t)index;
	list->last_clicked = index;
	list->last_click_ms = now_ms;
	list->has_last_click = true;
	return double_click ? SLOT_LIST_CLICK_ACTIVATE : SLOT_LIST_CLICK_SELECT;
}

void slotlist_render(const struct slotlist* list, slotlist_draw_row_fn draw_row,
					 void* context) {
	assert(list);
	assert(draw_row);
	int s = GFX_GUI_SCALE;
	int left = list->width / 2 - 110 * s;
	int row_y = list->top + 4 * s - list->amount_scrolled;

	gfx_scissor(true, 0, list->top, list->width, list->bottom - list->top);
	for(size_t index = 0; index < list->count; index++) {
		int y = row_y + (int)index * list->row_height;
		if(y + list->row_height <= list->top || y >= list->bottom)
			continue;

		if(index == list->selection) {
			/* Keep the two screens' existing CaveWii selection chrome. Beta's
			 * GuiSlot uses the same 220-unit outer band but different GL quads. */
			gfx_texture(false);
			gutil_texquad_col(left, y, 0, 0, 0, 0, 220 * s, list->row_height,
							  0x80, 0x80, 0x80, 255);
			gutil_texquad_col(left + s, y + s, 0, 0, 0, 0, 218 * s,
							  list->row_height - 2 * s, 0, 0, 0, 255);
			gfx_texture(true);
		}
		/* [740c583 GuiSlot.drawScreen]: drawSlot receives width/2-108, the
		 * row origin, and posZ-4. The callbacks retain each screen's ink. */
		draw_row(index, list->width / 2 - 108 * s, y,
				 list->row_height - 4 * s, context);
	}
	gfx_scissor(false, 0, 0, 0, 0);
}
