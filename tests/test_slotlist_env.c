/*
	Headless goldens for the shared Beta GuiSlot port and GuiButton hit test,
	plus the death-screen-migration dwell/odd-width additions.

	Expected values are HAND-COMPUTED from the decompile (or, for the two
	CaveWii-only additions below, from the documented formula), never
	obtained by running the implementation under test (the POISONED-GOLDEN
	rule):
	  - clamp/content math [740c583 GuiSlot.getContentHeight/bindAmountScrolled]
	  - row hit mapping     [740c583 GuiSlot.func_27256_c/drawScreen]
	  - double-click gate   [740c583 GuiSlot.drawScreen: selected row and <250ms]
	  - button AABB         [740c583 GuiButton.mousePressed]
	  - odd-width guard     gutil_button_even_width: w_units % 2 == 0
	  - dwell fill math     gutil_button_dwell_fill_px: (int)(w_units *
	                        GFX_GUI_SCALE * clamp(fill_frac, 0, 1)) -- the
	                        Wii-controller hold-to-confirm carve-out #2
	                        (Beta's GuiButton has no dwell state)
*/

#include <assert.h>
#include <stdio.h>

#include "game/gui/widget_slotlist.h"
#include "graphics/gfx_settings.h"
#include "graphics/gui_util.h"

static void test_scroll_clamping(void) {
	const int s = GFX_GUI_SCALE;
	struct slotlist list;

	/* viewport = 128 logical units; GuiSlot removes 4, so ten 36-unit rows
	 * have max scroll 360 - 124 = 236 logical units. Scroll buttons move
	 * posZ*2/3 = 24 logical units per press. */
	slotlist_reset(&list, 320 * s, 32 * s, 160 * s, 36 * s, 10, 0);
	assert(slotlist_amount_scrolled(&list) == 0);
	slotlist_scroll(&list, 1);
	assert(slotlist_amount_scrolled(&list) == 24 * s);
	for(int k = 0; k < 20; k++)
		slotlist_scroll(&list, 1);
	assert(slotlist_amount_scrolled(&list) == 236 * s);
	slotlist_scroll(&list, -1);
	assert(slotlist_amount_scrolled(&list) == 212 * s);
	for(int k = 0; k < 20; k++)
		slotlist_scroll(&list, -1);
	assert(slotlist_amount_scrolled(&list) == 0);

	/* GuiSlot halves a negative maximum before binding. Empty: (0-124)/2
	 * = -62; one row: (36-124)/2 = -44. These negative values center
	 * undersized content instead of pinning it to the top. */
	slotlist_reset(&list, 320 * s, 32 * s, 160 * s, 36 * s, 0,
				   SLOT_LIST_NO_SELECTION);
	assert(slotlist_amount_scrolled(&list) == -62 * s);
	slotlist_reset(&list, 320 * s, 32 * s, 160 * s, 36 * s, 1, 0);
	assert(slotlist_amount_scrolled(&list) == -44 * s);
}

static void test_click_mapping(void) {
	const int s = GFX_GUI_SCALE;
	struct slotlist list;
	slotlist_reset(&list, 320 * s, 32 * s, 160 * s, 36 * s, 10,
				   SLOT_LIST_NO_SELECTION);

	/* GuiSlot's horizontal band is width/2 +/- 110, inclusive. At scroll 0,
	 * y=top is still in the 4-unit header and maps to no row; top+4 is the
	 * first row pixel. The bottom test is inclusive in GuiSlot. */
	assert(slotlist_index_at(&list, 50 * s, 32 * s) == -1);
	assert(slotlist_index_at(&list, 270 * s, 32 * s) == -1);
	assert(slotlist_index_at(&list, 49 * s, 40 * s) == -1);
	assert(slotlist_index_at(&list, 271 * s, 40 * s) == -1);
	assert(slotlist_index_at(&list, 50 * s, 36 * s) == 0);
	assert(slotlist_index_at(&list, 270 * s, 71 * s) == 0);
	assert(slotlist_index_at(&list, 160 * s, 72 * s) == 1);
	assert(slotlist_index_at(&list, 160 * s, 160 * s) == 3);
	assert(slotlist_index_at(&list, 160 * s, 161 * s) == -1);
}

static void test_pointer_scroll(void) {
	const int s = GFX_GUI_SCALE;
	struct slotlist list;
	slotlist_reset(&list, 320 * s, 32 * s, 160 * s, 36 * s, 10, 0);
	slotlist_scroll(&list, 1);
	assert(slotlist_amount_scrolled(&list) == 24 * s);

	/* First held sample anchors; moving toward the top drags content down by
	 * 12 logical units. [740c583 GuiSlot.drawScreen initialClickY branch] */
	slotlist_pointer_scroll(&list, 100 * s, true);
	assert(slotlist_amount_scrolled(&list) == 24 * s);
	slotlist_pointer_scroll(&list, 88 * s, true);
	assert(slotlist_amount_scrolled(&list) == 36 * s);
	slotlist_pointer_scroll(&list, 120 * s, true);
	assert(slotlist_amount_scrolled(&list) == 4 * s);

	/* Release resets the anchor; an outside-viewport hold cannot scroll. */
	slotlist_pointer_scroll(&list, 120 * s, false);
	slotlist_pointer_scroll(&list, 200 * s, true);
	assert(slotlist_amount_scrolled(&list) == 4 * s);
}

static void test_click_and_double_click(void) {
	const int s = GFX_GUI_SCALE;
	struct slotlist list;
	slotlist_reset(&list, 320 * s, 32 * s, 160 * s, 36 * s, 10,
				   SLOT_LIST_NO_SELECTION);

	assert(slotlist_click_at(&list, 160 * s, 40 * s, 1000)
		   == SLOT_LIST_CLICK_SELECT);
	assert(slotlist_selected(&list) == 0);
	assert(slotlist_click_at(&list, 160 * s, 40 * s, 1249)
		   == SLOT_LIST_CLICK_ACTIVATE);
	/* Strictly less than 250 ms: equality is a new single click. */
	assert(slotlist_click_at(&list, 160 * s, 40 * s, 1499)
		   == SLOT_LIST_CLICK_SELECT);
	assert(slotlist_click_at(&list, 160 * s, 76 * s, 1500)
		   == SLOT_LIST_CLICK_SELECT);
	assert(slotlist_selected(&list) == 1);
	/* Controller selection is external to GuiSlot's pointer-click history;
	 * moving back to the last-clicked row must not manufacture a double click. */
	slotlist_select(&list, 0, false);
	assert(slotlist_click_at(&list, 160 * s, 76 * s, 1501)
		   == SLOT_LIST_CLICK_SELECT);

	/* Empty list never selects. A one-row list is centered: row zero begins
	 * at top+4-(-44) = logical y 80. */
	slotlist_reset(&list, 320 * s, 32 * s, 160 * s, 36 * s, 0,
				   SLOT_LIST_NO_SELECTION);
	assert(slotlist_click_at(&list, 160 * s, 80 * s, 2000)
		   == SLOT_LIST_CLICK_NONE);
	assert(slotlist_selected(&list) == SLOT_LIST_NO_SELECTION);
	slotlist_reset(&list, 320 * s, 32 * s, 160 * s, 36 * s, 1,
				   SLOT_LIST_NO_SELECTION);
	assert(slotlist_index_at(&list, 160 * s, 79 * s) == -1);
	assert(slotlist_index_at(&list, 160 * s, 80 * s) == 0);
	assert(slotlist_click_at(&list, 160 * s, 80 * s, 2000)
		   == SLOT_LIST_CLICK_SELECT);
}

static void test_button_hit(void) {
	const int s = GFX_GUI_SCALE;
	struct gutil_button_bounds b = gutil_button_rect(10, 20, 70);
	assert(b.x == 10 && b.y == 20);
	assert(b.width == 70 * s && b.height == 20 * s);

	/* GuiButton.mousePressed uses inclusive lower and exclusive upper bounds. */
	assert(gutil_button_hit(&b, 10.0F, 20.0F));
	assert(gutil_button_hit(&b, (float)(10 + 70 * s) - 0.25F,
						 (float)(20 + 20 * s) - 0.25F));
	assert(!gutil_button_hit(&b, 9.99F, 20.0F));
	assert(!gutil_button_hit(&b, (float)(10 + 70 * s), 20.0F));
	assert(!gutil_button_hit(&b, 10.0F, (float)(20 + 20 * s)));
}

/* gutil_button_even_width backs the odd-width guard in
 * gutil_button_draw_bg's split site (assert at the split, 7a10eb4's
 * recorded debt): true iff w_units halves losslessly (half*2 == w_units).
 * Every button width live on the shared helper today is even. */
static void test_button_even_width(void) {
	assert(gutil_button_even_width(200));  /* death/title/pause/create/rename */
	assert(gutil_button_even_width(150));  /* confirm/select-world/texpacks */
	assert(gutil_button_even_width(98));   /* pause achievements/stats */
	assert(gutil_button_even_width(70));   /* select-world rename/delete */
	assert(!gutil_button_even_width(201));
	assert(!gutil_button_even_width(99));
	assert(!gutil_button_even_width(1));
}

/* gutil_button_dwell_fill_px: the death screen's hold-to-confirm charge
 * width in physical pixels, (int)(w_units * GFX_GUI_SCALE * clamp(p,0,1)).
 * GFX_GUI_SCALE is 2 (gfx_settings.h), so w_units=200 (DEATH_BTN_W) gives a
 * 400-physical-pixel span; fractions below are exact binary floats so the
 * expected products are exact, no rounding ambiguity. */
static void test_button_dwell_fill(void) {
	/* t=0 (at rest -- the caller skips drawing entirely at this value, but
	 * the pure fill math itself is still exactly zero here). */
	assert(gutil_button_dwell_fill_px(200, 0.0F) == 0);
	/* mid. */
	assert(gutil_button_dwell_fill_px(200, 0.25F) == 100);
	assert(gutil_button_dwell_fill_px(200, 0.5F) == 200);
	assert(gutil_button_dwell_fill_px(200, 0.75F) == 300);
	/* full (and clamped-above-full). */
	assert(gutil_button_dwell_fill_px(200, 1.0F) == 400);
	assert(gutil_button_dwell_fill_px(200, 2.0F) == 400);
	/* clamped-below-zero. */
	assert(gutil_button_dwell_fill_px(200, -1.0F) == 0);
	/* a second width, so the width factor isn't coincidentally unchecked. */
	assert(gutil_button_dwell_fill_px(150, 0.5F) == 150);
	assert(gutil_button_dwell_fill_px(98, 1.0F) == 196);
	/* truncation, not rounding: 7*2*0.125 = 1.75 -> 1, never 2. */
	assert(gutil_button_dwell_fill_px(7, 0.125F) == 1);
}

int main(void) {
	test_scroll_clamping();
	test_click_mapping();
	test_pointer_scroll();
	test_click_and_double_click();
	test_button_hit();
	test_button_even_width();
	test_button_dwell_fill();
	fprintf(stderr, "test_slotlist_env: all OK\n");
	return 0;
}
