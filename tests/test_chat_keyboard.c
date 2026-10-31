/* B1a -- the chat composer's keyboard against Astra's binding visual spec,
 * docs/ui_design/chat-keyboard/SPEC.md (ruling
 * tmp/jobs/b1-chat-keyboard-0917/astra-ruling-1.raw, Q1 and Q5).
 *
 * THE DEFECT THIS GUARDS. The composer used to be a (44,236,552,236) panel:
 * bottom edge y=472 of 480, with the Space/Backspace row spanning y440..468.
 * SDTV action-safe is ~5% = 24 rows, so the bottom row sat INSIDE the overscan
 * band -- the concrete "not readable on a TV" bug. Every rect is now checked
 * against the SPEC title-safe rectangle (32,24,576,432), so a regression to
 * that geometry fails here, by name, before anything reaches a console.
 *
 * Pure test: it compiles source/game/gui/widget_osk.c and
 * source/game/gui/chat_composer_model.c directly (the test_sign_approved_flow
 * pattern) with boundary doubles for the draw/device edge, so the numbers --
 * not a rendered frame -- are what is asserted. The RENDERER is separately
 * exercised against the real screen in tests/test_ui_geometry.c.
 *
 * Part 6 is the isolation proof the brief requires: osk_sign_key_bounds is
 * compared against a FROZEN copy of the pre-B1a formula for every rect the
 * sign editor asks for, so the chat-scoped additions cannot have moved the
 * sign keyboard by one pixel. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game/gui/chat_composer_model.h"
#include "game/gui/widget_osk.h"
#include "graphics/gui_util.h"
#include "platform/gfx.h"
#include "platform/input.h"
#include "platform/input_owner.h"

/* ---- boundary doubles (draw/device edge only; the model is the target) --- */
struct tex_gfx texture_font, texture_controls, texture_gui_widgets,
	texture_pointer, texture_approved_controls;
void gfx_texture(bool enabled) { (void)enabled; }
void gfx_bind_texture(struct tex_gfx* t) { (void)t; }
void gutil_texquad_col(int x, int y, int tx, int ty, int sx, int sy, int w,
					   int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	(void)x; (void)y; (void)tx; (void)ty; (void)sx; (void)sy; (void)w;
	(void)h; (void)r; (void)g; (void)b; (void)a;
}
void gutil_texquad(int x, int y, int tx, int ty, int sx, int sy, int w, int h) {
	(void)x; (void)y; (void)tx; (void)ty; (void)sx; (void)sy; (void)w; (void)h;
}
void gutil_menu_focus_border(const struct gutil_button_bounds* b) { (void)b; }
int gutil_prompt_glyph(int x, int y, int size, int glyph) {
	(void)x; (void)y; (void)glyph;
	return size;
}
int gutil_font_width(const char* str, int scale) {
	return (int)strlen(str) * scale;
}
void gutil_text_rgb(int x, int y, const char* s, int scale, bool shadow,
					uint8_t r, uint8_t g, uint8_t b) {
	(void)x; (void)y; (void)s; (void)scale; (void)shadow; (void)r; (void)g;
	(void)b;
}
bool gutil_button_hit(const struct gutil_button_bounds* q, float x, float y) {
	return x >= (float)q->x && x < (float)(q->x + q->width) && y >= (float)q->y
		&& y < (float)(q->y + q->height);
}
/* The chat profile is owner-frame driven, so the tests drive PLAYER 0's frame
 * directly. `pressed_bits` still backs input_pressed for the SIGN profile,
 * which keeps its merged-mask fallback. */
static bool pressed_bits[IB_COUNT];
bool input_pressed(enum input_button b) { return pressed_bits[b]; }
static bool text_active;
static bool mouse_edges[IB_COUNT];
bool input_text_active(void) { return text_active; }
bool input_text_mouse_pressed(enum input_button b) { return mouse_edges[b]; }

/* A controllable PC character queue, so the text-path rules (the opener's own
 * key repeat, and ordered delivery of Enter/Escape) can be driven exactly. */
static char text_q[32];
static size_t text_head, text_tail;
bool input_text_poll(char* out) {
	if(text_head == text_tail)
		return false;
	*out = text_q[text_head++];
	return true;
}
static void text_push(const char* s) {
	for(; *s; s++)
		text_q[text_tail++] = *s;
}
static void text_clear(void) { text_head = text_tail = 0; }
static size_t text_left(void) { return text_tail - text_head; }

/* Player 0 for the chat tests: a press is also down; a hold is down only. */
static struct input_player_frame P0 = {.connected = true};

static void press(enum input_button b) {
	memset(pressed_bits, 0, sizeof(pressed_bits));
	memset(P0.pressed, 0, sizeof(P0.pressed));
	memset(P0.down, 0, sizeof(P0.down));
	pressed_bits[b] = true;
	P0.pressed[b] = P0.down[b] = true;
}
/* Down with no rising edge -- what a second frame of the same hold looks like. */
static void hold(enum input_button b) {
	memset(pressed_bits, 0, sizeof(pressed_bits));
	memset(P0.pressed, 0, sizeof(P0.pressed));
	memset(P0.down, 0, sizeof(P0.down));
	P0.down[b] = true;
}
static void press_none(void) {
	memset(pressed_bits, 0, sizeof(pressed_bits));
	memset(P0.pressed, 0, sizeof(P0.pressed));
	memset(P0.down, 0, sizeof(P0.down));
}

/* 60 ms frames: four are 240 ms (under the 250 ms delay) and five are 300 ms
 * (over it), one is under the 80 ms rate and two are over -- both boundaries are
 * crossed with margin, so no float rounding decides a test. */
#define FRAME 0.06F

static struct osk_chat_result step_dt(struct osk_state* o,
									  struct osk_chat_view* v, float dt) {
	struct osk_chat_result r;
	osk_chat_update_owned(o, v, dt, false, 0, 0, true, &P0, true, &r);
	return r;
}

static struct osk_chat_result step(struct osk_state* o,
								   struct osk_chat_view* v) {
	return step_dt(o, v, FRAME);
}

/* Open the composer's input state the way screen_chat_reset does, then give it
 * the one neutral frame every held button needs before it may act. */
static void arm(struct osk_state* o, struct osk_chat_view* v) {
	P0.connected = true;
	osk_chat_input_reset(v);
	press_none();
	step(o, v);
}

static bool same(const struct chat_rect* r, int x, int y, int w, int h) {
	return r->x == x && r->y == y && r->w == w && r->h == h;
}
static struct chat_rect of(const struct gutil_button_bounds* q) {
	struct chat_rect r = {q->x, q->y, q->width, q->height};
	return r;
}

/* ---- 1. the SPEC rect table, at 4:3 and at the Wii's 802x480 16:9 -------- */
static void test_rect_table(void) {
	struct chat_geometry g;
	/* "At 4:3, GUI size is 640x480 ... Wii 16:9 uses 802x480 GUI pixels ...
	 * Set ox=(width-640)/2: 0 at 640, 81 at 802." */
	const int offsets[] = {0, 81};
	for(size_t k = 0; k < sizeof(offsets) / sizeof(offsets[0]); k++) {
		int ox = offsets[k];
		chat_geometry(&g, ox);
		assert(same(&g.panel, ox + 44, 120, 552, 312));
		assert(same(&g.header, ox + 56, 132, 528, 24));
		assert(same(&g.input, ox + 56, 164, 528, 32));
		assert(same(&g.tabs, ox + 56, 204, 528, 32));
		assert(same(&g.grid, ox + 56, 244, 528, 128));
		assert(same(&g.utility, ox + 56, 380, 528, 40));
		assert(same(&g.keyboard, ox + 56, 204, 528, 216));
		assert(same(&g.history, ox + 44, 60, 552, 54));
		assert(same(&g.notice, ox + 44, 24, 552, 28));
		assert(same(&g.draft, ox + 64, 172, 416, 16));
		assert(same(&g.counter, ox + 488, 172, 88, 16));
		assert(g.title_x == ox + 56 && g.title_y == 136);
		assert(g.type_hint_x == ox + 152 && g.type_hint_y == 132);
		assert(g.hint_y == 132 && g.hint_right == ox + 584 && g.hint_gap == 16);
		assert(g.history_newest_y == 98);
		/* "panel adds 12 horizontal pixels and 24 bottom pixels of reserve" */
		assert(g.panel.x + 12 == g.header.x);
		assert(g.panel.y + g.panel.h == 432); /* Astra Q1: "bottom at 432" */
		/* The reserved bands do not touch each other or the panel:
		 * "notice band stays separate at y24..52; panel starts y120, history
		 * ends y114." */
		assert(g.notice.y + g.notice.h == 52);
		assert(g.history.y == 60 && g.history.y + g.history.h == 114);
		assert(!chat_rect_overlap(&g.notice, &g.history));
		assert(!chat_rect_overlap(&g.history, &g.panel));
		/* Draft and counter share the input line without colliding: "Draft
		 * clip is (64,172,416,16), with 8 pixels between draft and counter"
		 * and "reserve 88 pixels at x488..576". */
		assert(g.draft.x + g.draft.w + 8 == g.counter.x);
		assert(g.counter.x + g.counter.w == ox + 576);
		assert(!chat_rect_overlap(&g.draft, &g.counter));
		assert(chat_rect_inside(&g.input, &g.draft));
		assert(chat_rect_inside(&g.input, &g.counter));
		/* The keyboard envelope is exactly tabs..utility. */
		assert(chat_rect_inside(&g.keyboard, &g.tabs));
		assert(chat_rect_inside(&g.keyboard, &g.grid));
		assert(chat_rect_inside(&g.keyboard, &g.utility));
		assert(g.keyboard.y == g.tabs.y);
		assert(g.keyboard.y + g.keyboard.h == g.utility.y + g.utility.h);
		assert(chat_rect_inside(&g.panel, &g.keyboard));
		assert(chat_rect_inside(&g.panel, &g.header));
		assert(chat_rect_inside(&g.panel, &g.input));
	}
	puts("  chat kb: SPEC rect table exact at ox 0 and 81, bands disjoint 1/1");
}

/* ---- 2. THE REGRESSION GUARD: nothing may re-enter the overscan band ----- */
static void test_title_safe(void) {
	struct chat_geometry g;
	const int offsets[] = {0, 81};
	for(size_t k = 0; k < sizeof(offsets) / sizeof(offsets[0]); k++) {
		int ox = offsets[k];
		chat_geometry(&g, ox);
		/* "the project title-safe rectangle is (32,24,576,432), ending at
		 * (608,456)"; the 16:9 centred composition's stricter rect
		 * (113,24,576,432) is this same rect plus ox. */
		assert(same(&g.safe, ox + 32, 24, 576, 432));
		assert(g.safe.y + g.safe.h == 456);
		const struct chat_rect* parts[]
			= {&g.panel, &g.header, &g.input,   &g.tabs,  &g.grid,
			   &g.utility, &g.keyboard, &g.notice, &g.history, &g.draft,
			   &g.counter};
		for(size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++)
			assert(chat_rect_inside(&g.safe, parts[i]));
		/* THE OLD GEOMETRY MUST NEVER COME BACK. Panel (44,236,552,236) ended
		 * at y=472 and its last key row at y=468 of 480 -- both outside the
		 * title-safe rect. Assert the failing shape explicitly so the message
		 * names the defect. */
		struct chat_rect old_panel = {ox + 44, 236, 552, 236};
		struct chat_rect old_last_row = {ox + 56, 440, 528, 28};
		assert(!chat_rect_inside(&g.safe, &old_panel)
			   && "the pre-B1a panel ended at y=472, inside SDTV overscan");
		assert(!chat_rect_inside(&g.safe, &old_last_row)
			   && "the pre-B1a bottom key row ended at y=468, inside overscan");
		assert(g.panel.y + g.panel.h < old_panel.y + old_panel.h);
		/* Every key, plus the 2-px focus border it may grow, stays inside. */
		for(int page = 0; page < OSK_PAGES; page++)
			for(int row = 0; row < OSK_CHAT_ROWS; row++)
				for(int col = 0; col < osk_chat_row_count(page, row); col++) {
					struct gutil_button_bounds q;
					assert(osk_chat_key_bounds(ox, row, col, &q));
					struct chat_rect r = of(&q);
					assert(chat_rect_inside(&g.safe, &r));
					struct chat_rect focus = {r.x - 2, r.y - 2, r.w + 4,
											  r.h + 4};
					assert(chat_rect_inside(&g.safe, &focus)
						   && "a focused key's 2-px border leaves title-safe");
				}
		/* The bottom-most drawn pixel of the composer. */
		assert(g.utility.y + g.utility.h == 420);
		assert(g.utility.y + g.utility.h + 2 <= g.safe.y + g.safe.h);
	}
	puts("  chat kb: every rect + focus border inside title-safe; the y=472 / "
		 "y=468 overscan geometry is rejected 1/1");
}

/* ---- 3. the key grid: SPEC numbers, no overlaps, minimum sizes ----------- */
static void test_key_grid(void) {
	/* "Widths 49,49,49,49,50,49,49,49,49,50" and "Left positions:
	 * 56,109,162,215,268,322,375,428,481,534". */
	static const int lefts[10]
		= {56, 109, 162, 215, 268, 322, 375, 428, 481, 534};
	static const int widths[10] = {49, 49, 49, 49, 50, 49, 49, 49, 49, 50};
	static const int row_tops[3] = {244, 288, 332};
	struct gutil_button_bounds q;
	for(int row = OSK_CHAT_GRID_ROW_FIRST; row <= OSK_CHAT_GRID_ROW_LAST; row++)
		for(int col = 0; col < 10; col++) {
			assert(osk_chat_key_bounds(0, row, col, &q));
			assert(q.x == lefts[col] && q.width == widths[col]);
			assert(q.y == row_tops[row - 1] && q.height == 40);
			/* Astra Q1: "40-pixel key height"; the plan's floor was 49x34. */
			assert(q.width >= 49 && q.height >= 40);
			if(col > 0) {
				struct gutil_button_bounds prev;
				assert(osk_chat_key_bounds(0, row, col - 1, &prev));
				assert(q.x - (prev.x + prev.width) == 4); /* "Horizontal gap 4" */
			}
		}
	/* Vertical gap 4 between the three character rows. */
	assert(row_tops[1] - (row_tops[0] + 40) == 4);
	assert(row_tops[2] - (row_tops[1] + 40) == 4);
	/* "Three tabs: Letters (56,204,172,32), Numbers (234,204,172,32), Symbols
	 * (412,204,172,32); gaps 6 pixels." */
	static const int tab_x[OSK_CHAT_TABS] = {56, 234, 412};
	for(int col = 0; col < OSK_CHAT_TABS; col++) {
		assert(osk_chat_key_bounds(0, OSK_CHAT_ROW_TABS, col, &q));
		assert(q.x == tab_x[col] && q.y == 204 && q.width == 172
			   && q.height == 32);
	}
	assert(tab_x[1] - (tab_x[0] + 172) == 6);
	assert(tab_x[2] - (tab_x[1] + 172) == 6);
	assert(tab_x[2] + 172 == 584); /* the 528-wide envelope's right edge */
	/* "Utility row: case (56,380,84,40), Space (144,380,220,40), Backspace
	 * (368,380,216,40)" with gaps 4. */
	static const int util_x[OSK_CHAT_UTILS] = {56, 144, 368};
	static const int util_w[OSK_CHAT_UTILS] = {84, 220, 216};
	for(int col = 0; col < OSK_CHAT_UTILS; col++) {
		assert(osk_chat_key_bounds(0, OSK_CHAT_ROW_UTILITY, col, &q));
		assert(q.x == util_x[col] && q.y == 380 && q.width == util_w[col]
			   && q.height == 40);
	}
	assert(util_x[1] - (util_x[0] + util_w[0]) == 4);
	assert(util_x[2] - (util_x[1] + util_w[1]) == 4);
	assert(util_x[2] + util_w[2] == 584);
	/* No two live keys overlap, on any page. */
	for(int page = 0; page < OSK_PAGES; page++) {
		struct chat_rect all[OSK_CHAT_ROWS * OSK_COLS];
		size_t n = 0;
		for(int row = 0; row < OSK_CHAT_ROWS; row++)
			for(int col = 0; col < osk_chat_row_count(page, row); col++) {
				struct gutil_button_bounds b;
				assert(osk_chat_key_bounds(0, row, col, &b));
				all[n++] = of(&b);
			}
		for(size_t i = 0; i < n; i++)
			for(size_t j = i + 1; j < n; j++)
				assert(!chat_rect_overlap(&all[i], &all[j]));
	}
	/* Each row sits inside the envelope the SPEC table gives it. */
	struct chat_geometry g;
	chat_geometry(&g, 0);
	for(int row = 0; row < OSK_CHAT_ROWS; row++)
		for(int col = 0; col < osk_chat_row_count(OSK_PAGE_LOWER, row); col++) {
			struct gutil_button_bounds b;
			assert(osk_chat_key_bounds(0, row, col, &b));
			struct chat_rect r = of(&b);
			const struct chat_rect* env = row == OSK_CHAT_ROW_TABS ? &g.tabs
				: row == OSK_CHAT_ROW_UTILITY				  ? &g.utility
															  : &g.grid;
			assert(chat_rect_inside(env, &r));
			assert(chat_rect_inside(&g.keyboard, &r));
		}
	puts("  chat kb: 49/50x40 cells at the SPEC lefts, 172x32 tabs, "
		 "84/220/216 utilities, no overlaps 1/1");
}

/* ---- 4. pages, tabs and the case target --------------------------------- */
static void test_pages(void) {
	assert(osk_chat_tab_of_page(OSK_PAGE_LOWER) == OSK_CHAT_TAB_LETTERS);
	assert(osk_chat_tab_of_page(OSK_PAGE_UPPER) == OSK_CHAT_TAB_LETTERS);
	assert(osk_chat_tab_of_page(OSK_PAGE_NUM) == OSK_CHAT_TAB_NUMBERS);
	assert(osk_chat_tab_of_page(OSK_PAGE_SYMBOLS) == OSK_CHAT_TAB_SYMBOLS);
	assert(osk_chat_page_of_tab(OSK_CHAT_TAB_LETTERS, false) == OSK_PAGE_LOWER);
	assert(osk_chat_page_of_tab(OSK_CHAT_TAB_LETTERS, true) == OSK_PAGE_UPPER);
	assert(osk_chat_page_of_tab(OSK_CHAT_TAB_NUMBERS, true) == OSK_PAGE_NUM);
	assert(osk_chat_page_of_tab(OSK_CHAT_TAB_SYMBOLS, true) == OSK_PAGE_SYMBOLS);
	/* "disabled on Numbers/Symbols"; "Case label ABC while lower, abc while
	 * upper". */
	assert(osk_chat_case_enabled(OSK_PAGE_LOWER));
	assert(osk_chat_case_enabled(OSK_PAGE_UPPER));
	assert(!osk_chat_case_enabled(OSK_PAGE_NUM));
	assert(!osk_chat_case_enabled(OSK_PAGE_SYMBOLS));
	assert(strcmp(osk_chat_case_label(false), "ABC") == 0);
	assert(strcmp(osk_chat_case_label(true), "abc") == 0);
	assert(strcmp(osk_chat_tab_label(OSK_CHAT_TAB_LETTERS), "Letters") == 0);
	assert(strcmp(osk_chat_tab_label(OSK_CHAT_TAB_NUMBERS), "Numbers") == 0);
	assert(strcmp(osk_chat_tab_label(OSK_CHAT_TAB_SYMBOLS), "Symbols") == 0);
	/* The SPEC page table, character for character -- it is the EXISTING
	 * osk_sign_page_char ordering, reused, not a second table. */
	static const char* const rows[OSK_PAGES][3] = {
		[OSK_PAGE_LOWER] = {"qwertyuiop", "asdfghjkl;", "zxcvbnm,./"},
		[OSK_PAGE_UPPER] = {"QWERTYUIOP", "ASDFGHJKL;", "ZXCVBNM,./"},
		[OSK_PAGE_NUM] = {"1234567890", ".,!?-+/:;(", ")"},
		[OSK_PAGE_SYMBOLS] = {"\"#$%&'*<=>", "@[\\]^_`{|}", "~"},
	};
	for(int page = 0; page < OSK_PAGES; page++)
		for(int row = 1; row <= 3; row++) {
			const char* want = rows[page][row - 1];
			assert(osk_chat_row_count(page, row) == (int)strlen(want));
			for(int col = 0; want[col]; col++)
				assert(osk_sign_page_char(page, row, col) == want[col]);
		}
	/* "Empty cells have no button and cannot receive focus." */
	assert(!osk_chat_selectable(OSK_PAGE_NUM, 3, 1));
	assert(!osk_chat_selectable(OSK_PAGE_SYMBOLS, 3, 1));
	assert(osk_chat_selectable(OSK_PAGE_NUM, 3, 0));
	/* The disabled case key is not focusable either. */
	assert(osk_chat_selectable(OSK_PAGE_LOWER, OSK_CHAT_ROW_UTILITY,
							   OSK_CHAT_UTIL_CASE));
	assert(!osk_chat_selectable(OSK_PAGE_NUM, OSK_CHAT_ROW_UTILITY,
								OSK_CHAT_UTIL_CASE));
	assert(osk_chat_selectable(OSK_PAGE_NUM, OSK_CHAT_ROW_UTILITY,
							   OSK_CHAT_UTIL_SPACE));
	/* "All tabs persist on all pages." */
	for(int page = 0; page < OSK_PAGES; page++)
		assert(osk_chat_row_count(page, OSK_CHAT_ROW_TABS) == OSK_CHAT_TABS);
	puts("  chat kb: three tabs, case target, existing page ordering 1/1");
}

/* ---- 5. navigation, editing and the 95-character repertoire -------------- */
static void test_navigation(void) {
	char buf[CHAT_COMPOSER_CAP + 1] = "";
	struct osk_state o;
	struct osk_chat_view v = {0};
	osk_sign_reset(&o, buf, sizeof buf, CHAT_COMPOSER_CAP);
	o.page = OSK_PAGE_LOWER;
	o.row = OSK_CHAT_GRID_ROW_FIRST;
	o.col = 0;
	/* "Start on lowercase q, never Send." */
	assert(osk_sign_page_char(o.page, o.row, o.col) == 'q');
	press(IB_GUI_CLICK);
	struct osk_chat_result r = step(&o, &v);
	assert(r.activated && strcmp(buf, "q") == 0);
	assert(r.press_row == OSK_CHAT_GRID_ROW_FIRST && r.press_col == 0);
	/* Horizontal wrap (the existing rule). */
	press(IB_GUI_LEFT);
	step(&o, &v);
	assert(o.col == 9);
	press(IB_GUI_RIGHT);
	step(&o, &v);
	assert(o.col == 0);
	/* Vertical movement skips the disabled case key: from the grid's column 0
	 * down to the utility row, Space is the nearest selectable cell while the
	 * page is Numbers. */
	o.page = OSK_PAGE_NUM;
	o.row = OSK_CHAT_GRID_ROW_LAST;
	o.col = 0;
	press(IB_GUI_DOWN);
	step(&o, &v);
	assert(o.row == OSK_CHAT_ROW_UTILITY && o.col == OSK_CHAT_UTIL_SPACE);
	/* On Letters the same move reaches the enabled case key. */
	o.page = OSK_PAGE_LOWER;
	o.row = OSK_CHAT_GRID_ROW_LAST;
	o.col = 0;
	press(IB_GUI_DOWN);
	step(&o, &v);
	assert(o.row == OSK_CHAT_ROW_UTILITY && o.col == OSK_CHAT_UTIL_CASE);
	/* Case toggles, keeps the draft, and is remembered across tabs. */
	press(IB_GUI_CLICK);
	step(&o, &v);
	assert(v.upper && o.page == OSK_PAGE_UPPER && strcmp(buf, "q") == 0);
	o.row = OSK_CHAT_ROW_TABS;
	o.col = OSK_CHAT_TAB_NUMBERS;
	press(IB_GUI_CLICK);
	step(&o, &v);
	assert(o.page == OSK_PAGE_NUM && v.upper);
	o.row = OSK_CHAT_ROW_TABS;
	o.col = OSK_CHAT_TAB_LETTERS;
	press(IB_GUI_CLICK);
	step(&o, &v);
	assert(o.page == OSK_PAGE_UPPER && "Letters restores remembered case");
	/* Page shortcuts: the existing IB_GUI_PREVIOUS / IB_GUI_PAGE actions walk
	 * the tabs and "stop at ends, do not repeat". */
	o.page = OSK_PAGE_LOWER;
	v.upper = false;
	o.row = OSK_CHAT_GRID_ROW_FIRST;
	o.col = 3;
	press(IB_GUI_PREVIOUS);
	step(&o, &v);
	assert(o.page == OSK_PAGE_LOWER && "Letters is the first page: no wrap");
	press(IB_GUI_PAGE);
	step(&o, &v);
	assert(o.page == OSK_PAGE_NUM && o.row == OSK_CHAT_GRID_ROW_FIRST
		   && o.col == 3 && "focus row/column preserved where valid");
	press(IB_GUI_PAGE);
	step(&o, &v);
	assert(o.page == OSK_PAGE_SYMBOLS);
	press(IB_GUI_PAGE);
	step(&o, &v);
	assert(o.page == OSK_PAGE_SYMBOLS && "Symbols is the last page: no wrap");
	press(IB_GUI_PREVIOUS);
	step(&o, &v);
	assert(o.page == OSK_PAGE_NUM);
	/* A page change onto a shorter row clamps to the nearest existing column
	 * instead of stranding the cursor. */
	o.page = OSK_PAGE_LOWER;
	o.row = OSK_CHAT_GRID_ROW_LAST;
	o.col = 7;
	press(IB_GUI_PAGE);
	step(&o, &v);
	assert(o.page == OSK_PAGE_NUM && o.row == OSK_CHAT_GRID_ROW_LAST
		   && o.col == 0 && "clamped to the lone ')' cell");
	/* B backspaces from anywhere and reports the Backspace key as pressed. */
	o.page = OSK_PAGE_LOWER;
	o.row = OSK_CHAT_GRID_ROW_FIRST;
	o.col = 0;
	press(IB_GUI_CLICK_ALT);
	r = step(&o, &v);
	assert(buf[0] == 0 && r.press_row == OSK_CHAT_ROW_UTILITY
		   && r.press_col == OSK_CHAT_UTIL_BACK);
	/* Backspace on an empty draft is a no-op. */
	press(IB_GUI_CLICK_ALT);
	step(&o, &v);
	assert(buf[0] == 0);
	/* Every one of the 95 printable ASCII characters, Space included, is
	 * reachable from the three tabs (there is no keyboard fallback on a Wii). */
	bool seen[128] = {false};
	for(int tab = 0; tab < OSK_CHAT_TABS; tab++)
		for(int upper = 0; upper < 2; upper++) {
			int page = osk_chat_page_of_tab(tab, upper != 0);
			for(int row = 1; row <= 3; row++)
				for(int col = 0; col < osk_chat_row_count(page, row); col++) {
					char ch = osk_sign_page_char(page, row, col);
					if(ch)
						seen[(unsigned char)ch] = true;
				}
		}
	seen[' '] = true; /* the dedicated Space utility key */
	for(int ch = 32; ch < 127; ch++)
		assert(seen[ch] && "a printable ASCII character is unreachable");
	puts("  chat kb: wrap, case memory, page shortcuts, clamping, all 95 "
		 "printable ASCII 1/1");
}

/* ---- 6. the cap, and the /login replay ---------------------------------- */
static void test_cap_and_replay(void) {
	char buf[CHAT_COMPOSER_CAP + 1] = "";
	struct osk_state o;
	struct osk_chat_view v = {0};
	osk_sign_reset(&o, buf, sizeof buf, CHAT_COMPOSER_CAP);
	o.page = OSK_PAGE_LOWER;
	/* Type "/login hunter22" purely through key presses: '/' is on Letters row
	 * 3, the letters on rows 1-2, Space is the utility key and the digits are
	 * on the Numbers tab. SPEC: "`/login hunter22` is 15 characters. It is
	 * fictional preview text, never a stored real credential." */
	const char* want = "/login hunter22";
	for(const char* p = want; *p; p++) {
		bool placed = false;
		if(*p == ' ') {
			o.row = OSK_CHAT_ROW_UTILITY;
			o.col = OSK_CHAT_UTIL_SPACE;
			placed = true;
		} else {
			for(int tab = 0; tab < OSK_CHAT_TABS && !placed; tab++) {
				int page = osk_chat_page_of_tab(tab, false);
				for(int row = 1; row <= 3 && !placed; row++)
					for(int col = 0; col < osk_chat_row_count(page, row);
						col++)
						if(osk_sign_page_char(page, row, col) == *p) {
							o.page = page, o.row = row, o.col = col;
							placed = true;
							break;
						}
			}
		}
		assert(placed);
		press(IB_GUI_CLICK);
		step(&o, &v);
	}
	assert(strcmp(buf, want) == 0);
	assert(strlen(buf) == 15);
	/* The cap refuses the 101st byte with the buffer unchanged and reports the
	 * rejection so the counter can flash. */
	memset(buf, 'a', CHAT_COMPOSER_CAP);
	buf[CHAT_COMPOSER_CAP] = 0;
	o.page = OSK_PAGE_LOWER;
	o.row = OSK_CHAT_GRID_ROW_FIRST;
	o.col = 0;
	press(IB_GUI_CLICK);
	struct osk_chat_result r = step(&o, &v);
	assert(strlen(buf) == CHAT_COMPOSER_CAP && r.rejected);
	/* Space is a text-producing key and is refused the same way. */
	o.row = OSK_CHAT_ROW_UTILITY;
	o.col = OSK_CHAT_UTIL_SPACE;
	press(IB_GUI_CLICK);
	r = step(&o, &v);
	assert(strlen(buf) == CHAT_COMPOSER_CAP && r.rejected);
	/* Navigation is never refused at the cap, and Backspace still works. */
	o.row = OSK_CHAT_ROW_TABS;
	o.col = OSK_CHAT_TAB_NUMBERS;
	press(IB_GUI_CLICK);
	r = step(&o, &v);
	assert(o.page == OSK_PAGE_NUM && !r.rejected);
	press(IB_GUI_CLICK_ALT);
	step(&o, &v);
	assert(strlen(buf) == CHAT_COMPOSER_CAP - 1);
	press_none();
	puts("  chat kb: /login hunter22 replays to 15 bytes; the cap refuses "
		 "characters and Space but not navigation 1/1");
}

/* ---- 7. ISOLATION: the sign editor's keyboard did not move --------------- */

/* A FROZEN copy of osk_sign_key_bounds as it stood before B1a (widget_osk.c
 * :414-420 at base faf1d16f). The chat profile is additive, so the sign rects
 * must still come out byte-identical for every rect the sign editor asks for.
 * If this ever disagrees, a chat change leaked into a frozen entry point. */
static bool sign_bounds_frozen(int ox, int oy, int width, bool compact, int row,
							   int col, struct gutil_button_bounds* out) {
	if(!out || row < 0 || row > 4)
		return false;
	int cols = row == 0 ? 4 : row == 4 ? 2 : 10;
	if(col < 0 || col >= cols)
		return false;
	int gap = compact ? 2 : 4, rh = compact ? 25 : 28,
		usable = width - (cols - 1) * gap;
	int x0 = ox + col * gap + col * usable / cols,
		x1 = ox + col * gap + (col + 1) * usable / cols;
	*out = (struct gutil_button_bounds) {x0, oy + row * (rh + gap), x1 - x0, rh};
	return true;
}

static void test_sign_unchanged(void) {
	/* The two compositions the sign editor and the compact chat path use
	 * (screen_sign.c: full 552 wide, compact 330; screen_chat.c compact 552). */
	const int widths[] = {552, 528, 330};
	unsigned checked = 0;
	for(size_t w = 0; w < sizeof(widths) / sizeof(widths[0]); w++)
		for(int compact = 0; compact < 2; compact++)
			for(int ox = 0; ox <= 81; ox += 81)
				for(int oy = 0; oy <= 236; oy += 118)
					for(int row = 0; row < 5; row++)
						for(int col = 0; col < 10; col++) {
							struct gutil_button_bounds got, want;
							bool a = osk_sign_key_bounds(ox, oy, widths[w],
														 compact != 0, row, col,
														 &got);
							bool b = sign_bounds_frozen(ox, oy, widths[w],
														compact != 0, row, col,
														&want);
							assert(a == b);
							if(!a)
								continue;
							assert(got.x == want.x && got.y == want.y
								   && got.width == want.width
								   && got.height == want.height
								   && "the sign keyboard moved");
							checked++;
						}
	assert(checked > 0);
	/* The sign page table is untouched too: the chat profile READS it. */
	assert(osk_sign_page_char(OSK_PAGE_LOWER, 1, 0) == 'q');
	assert(osk_sign_page_char(OSK_PAGE_UPPER, 3, 9) == '/');
	assert(osk_sign_page_char(OSK_PAGE_NUM, 3, 0) == ')');
	assert(osk_sign_page_char(OSK_PAGE_SYMBOLS, 3, 0) == '~');
	assert(osk_sign_page_char(OSK_PAGE_SYMBOLS, 3, 1) == 0);
	/* The legacy grid profile (create-world / rename / server setup) is not
	 * touched either. */
	assert(osk_page_char(OSK_PAGE_LOWER, 1, 0) == 'q');
	assert(osk_page_char(OSK_PAGE_NUM, 1, 0) == '-');
	printf("  chat kb: %u sign key rects byte-identical to the frozen pre-B1a "
		   "formula; sign/legacy tables unchanged 1/1\n",
		   checked);
}

/* ---- 8. B1b: held-direction repeat, CHAT ONLY --------------------------- */
static void test_repeat(void) {
	char buf[CHAT_COMPOSER_CAP + 1] = "";
	struct osk_state o;
	struct osk_chat_view v = {0};
	osk_sign_reset(&o, buf, sizeof buf, CHAT_COMPOSER_CAP);
	o.page = OSK_PAGE_LOWER, o.row = OSK_CHAT_GRID_ROW_FIRST, o.col = 0;
	arm(&o, &v);
	/* SPEC "Motion": "Direction edge moves immediately". */
	press(IB_GUI_RIGHT);
	struct osk_chat_result r = step(&o, &v);
	assert(r.moved && !r.moved_repeat && o.col == 1);
	/* "first held repeat at 250 ms": four 50 ms frames of hold are silent. */
	for(int f = 0; f < 4; f++) {
		hold(IB_GUI_RIGHT);
		r = step(&o, &v);
		assert(!r.moved && o.col == 1 && "a held direction repeated early");
	}
	hold(IB_GUI_RIGHT);
	r = step(&o, &v);
	assert(r.moved && r.moved_repeat && o.col == 2 && "no repeat at 250 ms");
	/* "subsequent repeats 80 ms": one 50 ms frame is not enough, two are. */
	hold(IB_GUI_RIGHT);
	r = step(&o, &v);
	assert(!r.moved);
	hold(IB_GUI_RIGHT);
	r = step(&o, &v);
	assert(r.moved && r.moved_repeat && o.col == 3);
	/* "Never accumulate a burst of repeats after a slow frame": one long frame
	 * yields exactly ONE step, and the next interval is a full 80 ms again. */
	hold(IB_GUI_RIGHT);
	r = step_dt(&o, &v, 5.0F);
	assert(r.moved && o.col == 4);
	hold(IB_GUI_RIGHT);
	r = step(&o, &v);
	assert(!r.moved && o.col == 4 && "a slow frame banked repeats");
	/* "Any neutral frame resets it": after a release the next hold starts the
	 * 250 ms delay over instead of continuing at 80 ms. */
	press_none();
	r = step(&o, &v);
	assert(!r.moved && v.repeat.direction == 0);
	hold(IB_GUI_RIGHT);
	r = step(&o, &v);
	assert(r.moved && o.col == 5);
	for(int f = 0; f < 4; f++) {
		hold(IB_GUI_RIGHT);
		assert(!step(&o, &v).moved && "the delay did not restart after neutral");
	}
	/* A page change also resets the timer (SPEC), and page actions never
	 * repeat: holding GUI_PAGE past the delay must not walk the tabs. */
	press(IB_GUI_PAGE);
	r = step(&o, &v);
	assert(r.page_changed && o.page == OSK_PAGE_NUM && v.repeat.direction == 0);
	for(int f = 0; f < 12; f++) {
		hold(IB_GUI_PAGE);
		r = step(&o, &v);
		assert(!r.page_changed && o.page == OSK_PAGE_NUM
			   && "a held page action repeated");
	}
	press_none();
	puts("  chat kb: repeat = immediate edge, nothing before 250 ms, then "
		 "80 ms, no burst after a slow frame, neutral and page change reset 1/1");
}

/* ---- 9. the sign editor is still EDGE-ONLY ------------------------------ */
static void test_sign_edge_only(void) {
	char buf[32] = "";
	struct osk_state o;
	osk_sign_reset(&o, buf, sizeof buf, 15);
	o.page = OSK_PAGE_LOWER, o.row = 1, o.col = 0;
	press(IB_GUI_RIGHT);
	osk_sign_update(&o, 0, 0, 552, false, false, 0, 0, true, NULL);
	int after_press = o.col;
	assert(after_press == 1);
	/* Hold the same direction far past chat's 250 ms delay and its 80 ms rate:
	 * the sign keyboard must not move once. */
	for(int f = 0; f < 40; f++) {
		hold(IB_GUI_RIGHT);
		osk_sign_update(&o, 0, 0, 552, false, false, 0, 0, true, NULL);
		assert(o.col == after_press && "the SIGN editor grew key repeat");
	}
	press_none();
	puts("  chat kb: the sign editor stays edge-only under a 40-frame hold 1/1");
}

/* ---- 10. focus-cue counts ----------------------------------------------- */
static void test_focus_cue(void) {
	char buf[CHAT_COMPOSER_CAP + 1] = "";
	struct osk_state o;
	struct osk_chat_view v = {0};
	osk_sign_reset(&o, buf, sizeof buf, CHAT_COMPOSER_CAP);
	o.page = OSK_PAGE_LOWER, o.row = OSK_CHAT_GRID_ROW_FIRST, o.col = 0;
	arm(&o, &v);
	int fresh = 0, repeated = 0, silent = 0;
	for(int f = 0; f < 12; f++) {
		if(f == 0)
			press(IB_GUI_RIGHT);
		else
			hold(IB_GUI_RIGHT);
		struct osk_chat_result r = step(&o, &v);
		if(r.moved && !r.moved_repeat)
			fresh++;
		else if(r.moved)
			repeated++;
		else
			silent++;
	}
	/* The screen plays MENU_SOUND_FOCUS on `moved && !moved_repeat` only, so
	 * one press + eleven held frames is exactly ONE cue. */
	assert(fresh == 1 && repeated > 0 && silent > 0);
	/* No cue when the cursor cannot move: the Numbers page's third row holds
	 * exactly one key, so left/right there is a no-op. */
	press_none();
	step(&o, &v);
	o.page = OSK_PAGE_NUM;
	o.row = OSK_CHAT_GRID_ROW_LAST;
	o.col = 0;
	assert(osk_chat_row_count(o.page, o.row) == 1);
	press(IB_GUI_RIGHT);
	struct osk_chat_result r = step(&o, &v);
	assert(!r.moved && "a blocked move still asked for a focus cue");
	press(IB_GUI_LEFT);
	r = step(&o, &v);
	assert(!r.moved);
	press_none();
	puts("  chat kb: one focus cue per fresh press, none on held repeats, "
		 "none on a blocked move 1/1");
}

/* ---- 11. input hygiene: the neutral re-arm ------------------------------ */
static void test_hygiene(void) {
	char buf[CHAT_COMPOSER_CAP + 1] = "";
	struct osk_state o;
	struct osk_chat_view v = {0};
	osk_sign_reset(&o, buf, sizeof buf, CHAT_COMPOSER_CAP);
	o.page = OSK_PAGE_LOWER, o.row = OSK_CHAT_GRID_ROW_FIRST, o.col = 0;

	/* (a) The press that OPENED the composer never types. screen_chat_reset
	 * runs while that button is still down, so the first frames see it held
	 * and must do nothing. */
	P0.connected = true;
	osk_chat_input_reset(&v);
	for(int f = 0; f < 5; f++) {
		press(IB_GUI_CLICK);
		struct osk_chat_result r = step(&o, &v);
		assert(!r.activated && buf[0] == 0 && "the opening press typed");
	}
	press_none();
	step(&o, &v);              /* the neutral frame re-arms it */
	press(IB_GUI_CLICK);
	assert(step(&o, &v).activated && strcmp(buf, "q") == 0);

	/* (b) Send and Cancel obey the same re-arm, so the press that opened the
	 * screen cannot submit and a HELD Send cannot fire twice. */
	osk_chat_input_reset(&v);
	press(IB_GUI_QUICK_MOVE);
	osk_chat_input_tick(&v, &P0);
	assert(!osk_chat_send_edge(&v, &P0) && "a held Send submitted on open");
	press_none();
	osk_chat_input_tick(&v, &P0);
	press(IB_GUI_QUICK_MOVE);
	osk_chat_input_tick(&v, &P0);
	assert(osk_chat_send_edge(&v, &P0));
	hold(IB_GUI_QUICK_MOVE);   /* still down, no new edge */
	osk_chat_input_tick(&v, &P0);
	assert(!osk_chat_send_edge(&v, &P0) && "a held Send repeated");
	/* Cancel is IB_INVENTORY or IB_HOME, both re-armed. */
	press_none();
	osk_chat_input_tick(&v, &P0);
	press(IB_HOME);
	osk_chat_input_tick(&v, &P0);
	assert(osk_chat_cancel_edge(&v, &P0));

	/* (c) A disconnect freezes edits and requires neutral re-arming: a pad
	 * that comes back still holding A does not type with it. */
	struct osk_chat_result r;
	press_none();
	step(&o, &v);
	size_t len = strlen(buf);
	press(IB_GUI_CLICK);
	P0.connected = false;
	r = step(&o, &v);
	assert(!r.activated && strlen(buf) == len && "a disconnected pad typed");
	P0.connected = true;       /* back, A still down */
	r = step(&o, &v);
	assert(!r.activated && strlen(buf) == len && "resume acted on a held button");
	press_none();
	step(&o, &v);              /* the neutral frame */
	press(IB_GUI_CLICK);
	r = step(&o, &v);
	assert(r.activated && strlen(buf) == len + 1);
	press_none();
	puts("  chat kb: opening press inert, Send/Cancel re-armed, disconnect and "
		 "resume need a neutral frame 1/1");
}

/* ---- 12. owner isolation ------------------------------------------------ */
static void test_owner_isolation(void) {
	char buf[CHAT_COMPOSER_CAP + 1] = "";
	struct osk_state o;
	struct osk_chat_view v = {0};
	osk_sign_reset(&o, buf, sizeof buf, CHAT_COMPOSER_CAP);
	o.page = OSK_PAGE_LOWER, o.row = OSK_CHAT_GRID_ROW_FIRST, o.col = 0;
	/* The composer reads ONLY the frame it is handed. Player 1's frame -- and
	 * the merged global mask the sign profile still falls back to -- cannot
	 * reach it, which is what keeps a split session's second pad out. */
	struct input_player_frame other = {.connected = true};
	other.pressed[IB_GUI_CLICK] = other.down[IB_GUI_CLICK] = true;
	other.pressed[IB_GUI_RIGHT] = other.down[IB_GUI_RIGHT] = true;
	memset(pressed_bits, 0, sizeof(pressed_bits));
	pressed_bits[IB_GUI_CLICK] = pressed_bits[IB_GUI_RIGHT] = true;
	struct input_player_frame idle = {.connected = true};
	struct osk_chat_result r;
	for(int f = 0; f < 3; f++) {
		osk_chat_update_owned(&o, &v, FRAME, false, 0, 0, true, &idle, true, &r);
		assert(!r.activated && !r.moved && buf[0] == 0 && o.col == 0
			   && "another owner (or the merged mask) drove the composer");
	}
	/* Handed the OTHER frame, the very same presses do act -- so the isolation
	 * above is the frame choice, not a dead code path. */
	osk_chat_update_owned(&o, &v, FRAME, false, 0, 0, true, &other, true, &r);
	assert(r.moved || r.activated);
	memset(pressed_bits, 0, sizeof(pressed_bits));
	puts("  chat kb: only the owning player's frame drives the composer 1/1");
}

/* ---- 13. the production navigation cost of "/login hunter22" ------------- */
static void test_login_press_cost(void) {
	/* The recorded shortest button path, one entry per character: the moves
	 * and then the activation. Replaying it through the PRODUCTION navigation
	 * model must land exactly this draft -- if a navigation rule changes, the
	 * script lands somewhere else and the draft assert fails loudly rather
	 * than the cost silently drifting. */
	static const char* const script[] = {
		"LDD#", "LU#", "U#", "LLLLD#", "RRRU#", "LLDD#", "D#", "RULU#",
		"RU#", "LDD#", "LUU#", "LL#", "R#", "LLP#", "#",
	};
	char buf[CHAT_COMPOSER_CAP + 1] = "";
	struct osk_state o;
	struct osk_chat_view v = {0};
	osk_sign_reset(&o, buf, sizeof buf, CHAT_COMPOSER_CAP);
	o.page = OSK_PAGE_LOWER, o.row = OSK_CHAT_GRID_ROW_FIRST, o.col = 0;
	arm(&o, &v);
	int moves = 0, clicks = 0;
	for(size_t k = 0; k < sizeof(script) / sizeof(script[0]); k++)
		for(const char* p = script[k]; *p; p++) {
			enum input_button b = IB_GUI_CLICK;
			switch(*p) {
				case 'L': b = IB_GUI_LEFT; break;
				case 'R': b = IB_GUI_RIGHT; break;
				case 'U': b = IB_GUI_UP; break;
				case 'D': b = IB_GUI_DOWN; break;
				case 'P': b = IB_GUI_PAGE; break;
				default: break;
			}
			if(*p == '#')
				clicks++;
			else
				moves++;
			press(b);
			step(&o, &v);
			press_none();
			step(&o, &v); /* release between presses, as a player does */
		}
	assert(strcmp(buf, "/login hunter22") == 0
		   && "the recorded key path no longer spells the login command");
	assert(strlen(buf) == 15);
	/* The navigation COST, asserted so a regression is visible. */
	assert(moves == 38 && clicks == 15 && moves + clicks == 53);
	/* The submit path: exactly these bytes leave the composer, once. */
	char out[CHAT_COMPOSER_CAP + 1];
	assert(chat_composer_submit(buf, out, sizeof out) == 15);
	assert(strcmp(out, "/login hunter22") == 0);
	printf("  chat kb: /login hunter22 costs %d direction/page presses + %d "
		   "activations = %d, and submits 15 bytes 1/1\n",
		   moves, clicks, moves + clicks);
}

/* ---- 14. review HIGH: the opener's own key never types (PC) -------------- */
static void test_opener_never_types(void) {
	char buf[CHAT_COMPOSER_CAP + 1] = "";
	struct osk_state o;
	struct osk_chat_view v = {0};
	osk_sign_reset(&o, buf, sizeof buf, CHAT_COMPOSER_CAP);
	o.page = OSK_PAGE_LOWER, o.row = OSK_CHAT_GRID_ROW_FIRST, o.col = 0;
	P0.connected = true;
	/* On PC IB_CHAT is the 'T' key, and the composer arms the GLFW char
	 * callback right after its one-shot drain -- so key REPEAT for the still
	 * held T produces characters AFTER that drain. They must be discarded. */
	osk_chat_input_reset(&v);
	text_clear();
	for(int f = 0; f < 6; f++) {
		press_none();
		P0.down[IB_CHAT] = true;   /* the opener is still held */
		text_push("ttt");
		step(&o, &v);
		assert(buf[0] == 0 && "the opener key typed into the draft");
		assert(text_left() == 0 && "the swallowed queue was not drained");
	}
	/* Released: ordinary PC typing works again on the very next frame. */
	press_none();
	step(&o, &v);
	text_push("ab");
	step(&o, &v);
	assert(strcmp(buf, "ab") == 0);
	/* The pause-menu opener never touches IB_CHAT, so text is live at once. */
	buf[0] = 0;
	osk_chat_input_reset(&v);
	press_none();                  /* IB_CHAT not held: the gate clears */
	step(&o, &v);
	text_push("hi");
	step(&o, &v);
	assert(strcmp(buf, "hi") == 0 && "the pause opener blocked typing");
	text_clear();
	puts("  chat kb: a held opener key cannot type; release (or the pause "
		 "opener) restores PC typing 1/1");
}

/* ---- 14b. b1-chat-opener-0919: X opens, and that same X cannot send ------
 * On the GameCube pad the opener (IB_CHAT) and the composer's Send
 * (IB_GUI_QUICK_MOVE) are the SAME physical button, code 406 -- Astra gate 2
 * D1 accepted that on purpose ("one button to remember"). What makes it safe
 * is the neutral re-arm the composer already owns: osk_chat_input_reset
 * blocks every chat button on the frame the opening press is still down, and
 * osk_chat_input_tick lifts a block only once that button has been SEEN up.
 * This pins that the opening press can never become the send. */
static void test_opener_press_never_sends(void) {
	char buf[CHAT_COMPOSER_CAP + 1] = "";
	struct osk_state o;
	struct osk_chat_view v = {0};
	osk_sign_reset(&o, buf, sizeof buf, CHAT_COMPOSER_CAP);
	P0.connected = true;
	/* screen_chat_reset's state, taken while X is still physically down. */
	osk_chat_input_reset(&v);
	press_none();
	P0.pressed[IB_CHAT] = P0.pressed[IB_GUI_QUICK_MOVE] = true;
	P0.down[IB_CHAT] = P0.down[IB_GUI_QUICK_MOVE] = true;
	for(int f = 0; f < 4; f++) {
		osk_chat_input_tick(&v, &P0);
		assert(!osk_chat_send_edge(&v, &P0)
			   && "the press that opened the composer sent the draft");
		assert(!osk_chat_cancel_edge(&v, &P0));
		/* every later frame of the same hold carries no rising edge */
		P0.pressed[IB_CHAT] = P0.pressed[IB_GUI_QUICK_MOVE] = false;
	}
	/* Released: still nothing on the release itself. */
	press_none();
	osk_chat_input_tick(&v, &P0);
	assert(!osk_chat_send_edge(&v, &P0));
	/* A genuinely fresh press of the same button sends. */
	press(IB_GUI_QUICK_MOVE);
	osk_chat_input_tick(&v, &P0);
	assert(osk_chat_send_edge(&v, &P0) && "a fresh X press could not send");
	press_none();
	puts("  chat kb: the opener press cannot become Send -- release and a "
		 "fresh press are required (GameCube X = open + send) 1/1");
}

/* ---- 15. review MEDIUM: the text queue is applied IN ORDER --------------- */
static void test_text_alias_isolation(void) {
	char buf[CHAT_COMPOSER_CAP + 1] = "";
	struct osk_state o;
	struct osk_chat_view v = {0};
	osk_sign_reset(&o, buf, sizeof buf, CHAT_COMPOSER_CAP);
	o.page = OSK_PAGE_LOWER, o.row = OSK_CHAT_GRID_ROW_FIRST, o.col = 0;
	arm(&o, &v);
	text_active = true;
	text_clear();
	text_push("ab");
	step(&o, &v);
	/* The polled alias can arrive one frame after its character callback. */
	press(IB_GUI_CLICK_ALT);
	struct osk_chat_result r = step(&o, &v);
	assert(strcmp(buf, "ab") == 0 && !r.activated
		   && "a printable Backspace alias deleted queued text");
	press_none();
	step(&o, &v);
	press(IB_GUI_CLICK);
	osk_chat_update_owned(&o, &v, FRAME, true, 80, 264, true,
		&P0, true, &r);
	assert(strcmp(buf, "ab") == 0 && !r.activated
		   && "a printable click alias activated the hovered key");
	/* A real mouse click still selects exactly once, even if a keyboard
	 * alternative keeps the merged action down and supplies no fresh edge. */
	hold(IB_GUI_CLICK);
	mouse_edges[IB_GUI_CLICK] = true;
	osk_chat_update_owned(&o, &v, FRAME, true, 80, 264, true,
		&P0, true, &r);
	assert(strcmp(buf, "abq") == 0 && r.activated);
	mouse_edges[IB_GUI_CLICK] = false;
	osk_chat_update_owned(&o, &v, FRAME, true, 80, 264, true,
		&P0, true, &r);
	assert(strcmp(buf, "abq") == 0 && !r.activated);
	/* This mouse-only edge path must still honor opening/disconnect re-arm. */
	osk_chat_input_reset(&v);
	mouse_edges[IB_GUI_CLICK] = true;
	press(IB_GUI_CLICK);
	osk_chat_update_owned(&o, &v, FRAME, true, 80, 264, true,
		&P0, true, &r);
	assert(strcmp(buf, "abq") == 0 && !r.activated);
	P0.connected = false;
	osk_chat_update_owned(&o, &v, FRAME, true, 80, 264, true,
		&P0, true, &r);
	assert(strcmp(buf, "abq") == 0 && !r.activated);
	P0.connected = true;
	osk_chat_update_owned(&o, &v, FRAME, true, 80, 264, true,
		&P0, true, &r);
	assert(strcmp(buf, "abq") == 0 && !r.activated);
	mouse_edges[IB_GUI_CLICK] = false;
	press_none(); step(&o, &v);
	press(IB_GUI_CLICK); mouse_edges[IB_GUI_CLICK] = true;
	osk_chat_update_owned(&o, &v, FRAME, true, 80, 264, true,
		&P0, true, &r);
	assert(strcmp(buf, "abqq") == 0 && r.activated);
	mouse_edges[IB_GUI_CLICK] = false;
	text_active = false;
	text_clear();
	press_none();
	puts("  chat kb: delayed printable click/back aliases isolated; mouse preserved");
}

static void test_text_queue_order(void) {
	char buf[CHAT_COMPOSER_CAP + 1] = "";
	struct osk_state o;
	struct osk_chat_view v = {0};
	osk_sign_reset(&o, buf, sizeof buf, CHAT_COMPOSER_CAP);
	o.page = OSK_PAGE_LOWER, o.row = OSK_CHAT_GRID_ROW_FIRST, o.col = 0;
	arm(&o, &v);
	/* ['\r','a']: the confirm wins and NOTHING after it is applied -- the old
	 * "last meaningful event wins" drain reported CHANGED and typed the 'a'. */
	text_clear();
	text_push("\ra");
	struct osk_chat_result r = step(&o, &v);
	assert(r.event == OSK_EV_DONE && "Enter was dropped by the drain");
	assert(buf[0] == 0 && "text after Enter still reached the draft");
	/* [27,'x']: the cancel wins, the draft is untouched. */
	text_clear();
	text_push("\033x");
	r = step(&o, &v);
	assert(r.event == OSK_EV_CANCEL && buf[0] == 0);
	/* Ordinary characters still all arrive, in order. */
	text_clear();
	text_push("abc");
	r = step(&o, &v);
	assert(strcmp(buf, "abc") == 0 && r.event == OSK_EV_CHANGED);
	text_clear();
	puts("  chat kb: the text queue applies in order and stops at the first "
		 "confirm/cancel 1/1");
}

/* ---- 16. review MEDIUM: A and B obey the re-arm for the SCREEN too ------- */
static void test_screen_edges_rearmed(void) {
	struct osk_chat_view v = {0};
	P0.connected = true;
	osk_chat_input_reset(&v);
	/* Each button is checked from its OWN open, because the re-arm blocks only
	 * what was already down when the composer took over -- a button that was
	 * neutral at open is free straight away, by design. */
	struct {
		enum input_button button;
		bool (*edge)(struct osk_chat_view*, const struct input_player_frame*);
		const char* why;
	} cases[] = {
		{IB_GUI_CLICK, osk_chat_click_edge, "a held A drove the pointer Send"},
		{IB_GUI_CLICK_ALT, osk_chat_back_edge, "a held B deleted on open"},
	};
	for(size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
		osk_chat_input_reset(&v);
		/* Held across the open: dark, however many frames pass. */
		for(int f = 0; f < 3; f++) {
			press(cases[k].button);
			osk_chat_input_tick(&v, &P0);
			assert(!cases[k].edge(&v, &P0) && cases[k].why);
		}
		/* Seen up once, then pressed again: live. */
		press_none();
		osk_chat_input_tick(&v, &P0);
		press(cases[k].button);
		osk_chat_input_tick(&v, &P0);
		assert(cases[k].edge(&v, &P0) && "the re-arm never released the button");
		/* Still down with no new edge: no repeat. */
		hold(cases[k].button);
		osk_chat_input_tick(&v, &P0);
		assert(!cases[k].edge(&v, &P0) && "a held button re-fired");
	}
	press_none();
	puts("  chat kb: the screen's A/B edges go through the same re-arm 1/1");
}

/* ---- 17. review LOW: a page tap while a direction is held --------------- */
static void test_page_tap_while_held(void) {
	char buf[CHAT_COMPOSER_CAP + 1] = "";
	struct osk_state o;
	struct osk_chat_view v = {0};
	osk_sign_reset(&o, buf, sizeof buf, CHAT_COMPOSER_CAP);
	o.page = OSK_PAGE_LOWER, o.row = OSK_CHAT_GRID_ROW_FIRST, o.col = 0;
	arm(&o, &v);
	press(IB_GUI_RIGHT);
	step(&o, &v);                       /* the edge moves once */
	int col = o.col;
	hold(IB_GUI_RIGHT);
	step(&o, &v);
	assert(o.col == col);
	/* Tap Page with the direction still down. The page changes and the repeat
	 * TIMER restarts -- but the held direction must not be re-read as a fresh
	 * press on the following frames. */
	memset(P0.pressed, 0, sizeof(P0.pressed));
	P0.pressed[IB_GUI_PAGE] = true;
	P0.down[IB_GUI_PAGE] = true;
	P0.down[IB_GUI_RIGHT] = true;
	struct osk_chat_result r = step(&o, &v);
	assert(r.page_changed && o.page == OSK_PAGE_NUM);
	int after = o.col;
	for(int f = 0; f < 4; f++) {
		hold(IB_GUI_RIGHT);
		r = step(&o, &v);
		assert(!r.moved && o.col == after
			   && "a page tap made the held direction move with no new press");
	}
	press_none();
	puts("  chat kb: tapping Page mid-hold restarts the timer without a "
		 "phantom step 1/1");
}

/* ---- 18. claims the review called untested ------------------------------ */
static void test_review_claims(void) {
	/* (a) The controller-mode request is a plain core-owned flag: the app-only
	 * bridge writes it, the composer reads it. Proving it here is what makes
	 * the mode exist in the shipped binary at all -- a direct call to the
	 * bridge compiled out of cavex_prod_core. */
	assert(!chat_controller_mode_requested());
	chat_controller_mode_set(true);
	assert(chat_controller_mode_requested());
	chat_controller_mode_set(false);
	assert(!chat_controller_mode_requested() && "RESET must clear the request");

	/* (b) Two directions held at once resolve deterministically: a fresh press
	 * wins over a hold, and a conflicting pair always picks the same one. */
	char buf[CHAT_COMPOSER_CAP + 1] = "";
	struct osk_state o;
	struct osk_chat_view v = {0};
	osk_sign_reset(&o, buf, sizeof buf, CHAT_COMPOSER_CAP);
	o.page = OSK_PAGE_LOWER, o.row = OSK_CHAT_GRID_ROW_FIRST, o.col = 4;
	arm(&o, &v);
	press(IB_GUI_RIGHT);
	step(&o, &v);
	assert(o.col == 5);
	/* RIGHT still down, LEFT freshly pressed -> the fresh press wins. */
	memset(P0.pressed, 0, sizeof(P0.pressed));
	P0.pressed[IB_GUI_LEFT] = true;
	P0.down[IB_GUI_LEFT] = P0.down[IB_GUI_RIGHT] = true;
	step(&o, &v);
	assert(o.col == 4 && "a fresh press lost to a held direction");
	/* Both merely held, neither fresh: the same direction wins every time. */
	int first = -1;
	for(int trial = 0; trial < 3; trial++) {
		struct osk_state t = o;
		struct osk_chat_view w = v;
		press_none();
		P0.down[IB_GUI_LEFT] = P0.down[IB_GUI_RIGHT] = true;
		for(int f = 0; f < 8; f++)
			osk_chat_update_owned(&t, &w, FRAME, false, 0, 0, true, &P0, true,
								  &(struct osk_chat_result) {0});
		if(first < 0)
			first = t.col;
		assert(t.col == first && "a conflicting direction pair is not stable");
	}
	/* (c) The pointer re-arms too: a pointer hit selects a key, but the click
	 * that lands it is still the re-armed A edge. */
	press_none();
	osk_chat_input_reset(&v);
	struct gutil_button_bounds q;
	assert(osk_chat_key_bounds(0, OSK_CHAT_GRID_ROW_FIRST, 3, &q));
	float px = (float)(q.x + q.width / 2), py = (float)(q.y + q.height / 2);
	press(IB_GUI_CLICK);
	struct osk_chat_result r;
	osk_chat_update_owned(&o, &v, FRAME, true, px, py, true, &P0, true, &r);
	assert(o.col == 3 && "the pointer did not select the hovered key");
	assert(!r.activated && "a held A clicked through the pointer");
	press_none();
	osk_chat_update_owned(&o, &v, FRAME, true, px, py, true, &P0, true, &r);
	press(IB_GUI_CLICK);
	osk_chat_update_owned(&o, &v, FRAME, true, px, py, true, &P0, true, &r);
	assert(r.activated && "the re-armed pointer click never fired");
	press_none();
	puts("  chat kb: core-owned controller-mode flag, deterministic direction "
		 "conflicts, re-armed pointer click 1/1");
}

/* ---- 19. review LOW: clipping never halves a Beta colour pair ----------- */
static int clip_w6(const char* s) {
	/* 6 px per visible char; a 0xA7 pair is zero-width, like the real font. */
	int w = 0, skip = 0;
	for(; *s; s++) {
		if((unsigned char)*s == 0xA7)
			skip = 2;
		if(skip > 0)
			skip--;
		else
			w += 6;
	}
	return w;
}

static void test_history_clip(void) {
	char out[CHAT_LINE_BYTES];
	/* A line whose cut lands exactly ON the pair: the old byte-wise clip left
	 * a dangling 0xA7 that would swallow the next glyph it met. */
	const char* line = "abcd\xA7""e";
	assert(clip_w6(line) == 24);
	assert(chat_clip_display(line, out, sizeof out, 24, clip_w6) == 6);
	assert(strcmp(out, line) == 0 && "a fitting line was modified");
	/* Force the cut to bite: 18 px keeps "abc" and must drop the whole pair. */
	size_t n = chat_clip_display(line, out, sizeof out, 18, clip_w6);
	assert(n == 3 && strcmp(out, "abc") == 0);
	assert((unsigned char)out[n - 1] != 0xA7 && "a half colour pair survived");
	/* A trailing lone opener is dropped even when it already fits. */
	assert(chat_clip_display("ab\xA7", out, sizeof out, 99, clip_w6) == 2);
	assert(strcmp(out, "ab") == 0);
	/* An 0xA7 that is the SELECTOR of an earlier pair is not an opener: the
	 * naive "is the last byte 0xA7" test would wrongly strip this one. */
	const char* sel = "x\xA7\xA7";
	assert(chat_clip_display(sel, out, sizeof out, 99, clip_w6) == 3);
	assert(strcmp(out, sel) == 0 && "a selector byte was mistaken for an opener");
	/* Degenerate inputs stay safe. */
	assert(chat_clip_display(NULL, out, sizeof out, 24, clip_w6) == 0);
	assert(chat_clip_display("abc", out, sizeof out, 0, clip_w6) == 0);
	puts("  chat kb: the open history clip never leaves half a colour pair 1/1");
}


/* The standalone keyboard target does not link the screen/renderer. Guard the
 * wiring here; test_chat_login_script executes its real footer layout below. */
static void test_screen_footer_wiring(void) {
	char path[1024];
	snprintf(path, sizeof path, "%s", __FILE__);
	char* base = strstr(path, "tests");
	assert(base);
	strcpy(base, "source/game/gui/screen_chat.c");
	FILE* f = fopen(path, "rb");
	assert(f);
	assert(fseek(f, 0, SEEK_END) == 0);
	long size = ftell(f);
	assert(size > 0 && fseek(f, 0, SEEK_SET) == 0);
	char* src = malloc((size_t)size + 1);
	assert(src && fread(src, 1, size, f) == (size_t)size);
	src[size] = 0;
	fclose(f);
	assert(strstr(src, "GUTIL_HINT_ALIGN_CENTRE"));
	assert(strstr(src, "gutil_hint_row_hit(&l.hints"));
	assert(strstr(src, "gutil_hint_row_draw(&l.hints)"));
	assert(strstr(src, "chat_row_key(&l, decision.target, &key)"));
	assert(!strstr(src, "gutil_prompt_glyph("));
	assert(!strstr(src, "gutil_menu_hint("));
	free(src);
	puts("  chat kb: centred clickable footer wiring, no in-panel glyph calls 1/1");
}

/* Pointer page/backspace activations must keep the keyboard's existing edit,
 * re-arm and queue precedence rules when the footer forwards to these keys. */
static void test_footer_key_activation(void) {
	char buf[CHAT_COMPOSER_CAP + 1] = "abc";
	struct osk_state o;
	struct osk_chat_view v = {0};
	osk_sign_reset(&o, buf, sizeof buf, CHAT_COMPOSER_CAP);
	strcpy(buf, "abc");
	arm(&o, &v);
	const int rows[] = {OSK_CHAT_ROW_TABS, OSK_CHAT_ROW_TABS, OSK_CHAT_ROW_UTILITY};
	const int cols[] = {1, 0, OSK_CHAT_UTIL_BACK};
	for(int i = 0; i < 3; i++) {
		struct gutil_button_bounds q;
		assert(osk_chat_key_bounds(0, rows[i], cols[i], &q));
		press(IB_GUI_CLICK);
		struct osk_chat_result r;
		osk_chat_update_owned(&o, &v, FRAME, true, q.x + 1, q.y + 1,
			true, &P0, true, &r);
		assert(r.activated);
		if(i < 2) assert(osk_chat_tab_of_page(o.page) == cols[i]);
		else assert(!strcmp(buf, "ab"));
		hold(IB_GUI_CLICK);
		osk_chat_update_owned(&o, &v, FRAME, true, q.x + 1, q.y + 1,
			true, &P0, true, &r);
		assert(!r.activated);
		press_none(); step(&o, &v);
	}
	puts("  chat kb: footer page/backspace clicks activate once through the keyboard 1/1");
}

int main(void) {
	puts("test_chat_keyboard (docs/ui_design/chat-keyboard/SPEC.md)");
	test_rect_table();
	test_title_safe();
	test_key_grid();
	test_pages();
	test_navigation();
	test_cap_and_replay();
	test_sign_unchanged();
	test_repeat();
	test_sign_edge_only();
	test_focus_cue();
	test_hygiene();
	test_owner_isolation();
	test_login_press_cost();
	test_opener_never_types();
	test_opener_press_never_sends();
	test_text_queue_order();
	test_text_alias_isolation();
	test_screen_edges_rearmed();
	test_page_tap_while_held();
	test_review_claims();
	test_history_clip();
	test_screen_footer_wiring();
	test_footer_key_activation();
	puts("test_chat_keyboard: all PASS");
	return 0;
}
