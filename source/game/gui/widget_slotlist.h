/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#ifndef WIDGET_SLOTLIST_H
#define WIDGET_SLOTLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SLOT_LIST_NO_SELECTION SIZE_MAX

enum slotlist_click_result {
	SLOT_LIST_CLICK_NONE,
	SLOT_LIST_CLICK_SELECT,
	SLOT_LIST_CLICK_ACTIVATE,
};

struct slotlist {
	int width;
	int top;
	int bottom;
	int row_height;
	int amount_scrolled;
	size_t count;
	size_t selection;
	int last_clicked;
	int64_t last_click_ms;
	bool has_last_click;
	float pointer_y;
	bool pointer_scrolling;
};

typedef void (*slotlist_draw_row_fn)(size_t index, int x, int y,
									 int row_height, void* context);

/* GuiSlot's viewport/content contract [740c583 GuiSlot.GuiSlot/drawScreen].
 * Dimensions are physical pixels. row_height is the already-scaled posZ. */
void slotlist_reset(struct slotlist* list, int width, int top, int bottom,
					int row_height, size_t count, size_t selection);
void slotlist_set_count(struct slotlist* list, size_t count);
void slotlist_select(struct slotlist* list, size_t selection, bool reveal);
size_t slotlist_selected(const struct slotlist* list);
int slotlist_amount_scrolled(const struct slotlist* list);

/* direction -1/+1 maps to GuiSlot's scroll-up/down buttons. */
void slotlist_scroll(struct slotlist* list, int direction);
/* Pointer-held row dragging with GuiSlot's 1:1 content motion. Call every
 * frame; held=false ends the drag. Scrollbar acceleration is intentionally
 * omitted. [740c583 GuiSlot.drawScreen] */
void slotlist_pointer_scroll(struct slotlist* list, float y, bool held);

/* Pure pointer seams used by both screens and the headless golden test. */
int slotlist_index_at(const struct slotlist* list, float x, float y);
enum slotlist_click_result slotlist_click_at(struct slotlist* list, float x,
										 float y, int64_t now_ms);

/* Scissor, visible-row culling, CaveWii's existing selection chrome, then the
 * caller's row callback. [740c583 GuiSlot.drawScreen] */
void slotlist_render(const struct slotlist* list, slotlist_draw_row_fn draw_row,
					 void* context);

#endif
