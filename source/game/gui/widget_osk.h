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

#ifndef WIDGET_OSK_H
#define WIDGET_OSK_H

#include <stdbool.h>
#include <stddef.h>

#include "../../graphics/menu_focus.h" /* struct gutil_menu_repeat (chat repeat) */

/* On-screen keyboard widget (design: docs/ui_design/osk_create_world_2026-08-15.md).
 *
 * A Wii has no keyboard, so text entry is the SECOND sanctioned carve-out
 * (controller playability): the EDIT SEMANTICS stay Beta's GuiTextField
 * [740c583 GuiTextField.textboxKeyTyped] -- append-only, backspace removes
 * the last char, printable filter, the max-length rule (`len < max || max
 * == 0`) -- while the key SOURCE is adapted. Every edit, whether an OSK
 * cell press or a PC physical keystroke, funnels through osk_feed_char();
 * the two input paths cannot diverge because there is only one mutator.
 *
 * The widget owns no buffer: the embedding screen supplies it and reads it
 * back directly. Embedders call osk_update() once per frame (input +
 * PC-char-queue drain) and osk_render() in their 2D pass. */

enum osk_event {
	OSK_EV_NONE,
	OSK_EV_CHANGED, /* buffer content changed */
	OSK_EV_NEXT,    /* Beta '\t': focus the next field  */
	OSK_EV_DONE,    /* Beta '\r': confirm the dialog    */
	OSK_EV_CANCEL,  /* ESC (27): close the screen [740c583 GuiScreen
					 * keyTyped key 1]. PC keyboard only -- the OSK grid
					 * has no cancel key; embedders map IB_INVENTORY. */
	OSK_EV_RANDOM,  /* frontend Create seed keypad "Random" (T05a): the parent
					 * fills a visible random seed into the DRAFT and rebinds */
};

/* Grid shape: 4 character rows x 10 columns + 1 function row. */
#define OSK_ROWS 4
#define OSK_COLS 10
#define OSK_FN_KEYS 6
#define OSK_PAGES 4

/* Page indices. OSK_PAGE_NUM is the numeric-first seed page
 * (docs/ui_design/create_world_console_2026-09-01.md section 3.2, maintainer
 * decision 4): Beta accepts ANY text in the seed field (the hashCode path),
 * but a seed is a number in practice, so the seed editor OPENS here instead
 * of costing a page switch. It holds the digits and '-' only -- exactly the
 * characters Java 6's Long.parseLong accepts. Shift/Sym leave it for the
 * letter/symbol pages (Shift's `page == 1 ? 0 : 1` lands on UPPER), and the
 * function row is unchanged: no "Num" key is added, because every digit and
 * '-' also live on pages 0/1, so nothing is stranded by the one-way trip
 * (test_osk_env S4 sweeps that reachability). */
#define OSK_PAGE_LOWER 0
#define OSK_PAGE_UPPER 1
#define OSK_PAGE_SYMBOLS 2
#define OSK_PAGE_NUM 3

struct osk_state {
	char* buf;      /* caller-owned, NUL-terminated */
	size_t buf_cap; /* total capacity incl. NUL */
	int beta_max;   /* Beta setMaxStringLength: 0 = unbounded (to buf_cap) */
	int page;       /* 0 lower, 1 upper (Shift), 2 symbols */
	int row;        /* 0..OSK_ROWS-1 = grid, OSK_ROWS = function row */
	int col;        /* grid: 0..OSK_COLS-1; function row: 0..OSK_FN_KEYS-1 */
};

void osk_reset(struct osk_state* o, char* buf, size_t buf_cap, int beta_max);

/* osk_reset, but opening on `page` (an OSK_PAGE_* index) with the cursor
 * parked on that page's first selectable cell. osk_reset is exactly
 * osk_reset_page(..., OSK_PAGE_LOWER). */
void osk_reset_page(struct osk_state* o, char* buf, size_t buf_cap,
					int beta_max, int page);

/* THE single mutator (the unit-test target). Accepts printable ASCII
 * 32..126 (append, subject to the Beta max rule and the buffer), '\b' (8,
 * backspace), '\t' (9, OSK_EV_NEXT), '\r'/'\n' (OSK_EV_DONE). Anything
 * else is ignored. */
enum osk_event osk_feed_char(struct osk_state* o, char c);

/* Per-frame input: IR pointer hover (overrides the d-pad cell, the
 * screen_sign precedence rule), IB_GUI_* cell movement with wraparound,
 * IB_GUI_CLICK presses the cell (only while `allow_click` -- the embedder
 * passes false when the pointer is over one of ITS controls so a shared
 * click edge is consumed exactly once), IB_GUI_CLICK_ALT backspaces from
 * anywhere. Also drains the PC physical-key queue (input_text_poll) into
 * osk_feed_char. (ox, oy) is the widget's top-left in PHYSICAL pixels. */
enum osk_event osk_update(struct osk_state* o, int ox, int oy, bool ptr,
						  float px, float py, bool allow_click);

void osk_render(const struct osk_state* o, int ox, int oy);

/* Same two entry points at an EXPLICIT scale. The approved compact (split)
 * sign editor has 240 rows to fit a header, a bounded sign face and the whole
 * keyboard in, which the fixed GFX_GUI_SCALE grid cannot do; the unscaled
 * calls above are exactly these with scale = GFX_GUI_SCALE, so both layouts
 * drive ONE cell table and ONE mutator (widget_osk.h's opening contract). */
enum osk_event osk_update_scaled(struct osk_state* o, int ox, int oy, int scale,
								 bool ptr, float px, float py,
								 bool allow_click);
void osk_render_scaled(const struct osk_state* o, int ox, int oy, int scale);

/* Widget footprint in LOGICAL units (multiply by GFX_GUI_SCALE). */
int osk_width(void);
int osk_height(void);

/* Page-table accessor (0 = blank cell). The test suite sweeps it to prove
 * every printable ASCII char is REACHABLE through the OSK -- on the Wii
 * there is no keyboard fallback, so a table typo would silently strand a
 * character. */
char osk_page_char(int page, int row, int col);

struct gutil_button_bounds;
struct input_player_frame;

/* ---- chat profile (docs/ui_design/chat-keyboard/SPEC.md) -----------------
 *
 * Astra's binding chat design, section "Keyboard grid and page states". It is
 * a PRESENTATION/NAVIGATION profile over the SAME character table and the SAME
 * single mutator: characters come from osk_sign_page_char and every edit still
 * goes through osk_feed_char. It is NOT a second text editor, and it is
 * strictly chat-scoped: `osk_sign_*` above -- entry points, tables, geometry
 * and every other caller's output -- are untouched by it.
 *
 * Rows: 0 = the three named tabs, 1..3 = the character grid, 4 = the utility
 * row (case / Space / Backspace). SPEC: "Navigation rows are tabs / character
 * rows / utility row; no separate Send/Cancel focus row is introduced." */
#define OSK_CHAT_ROWS 5
#define OSK_CHAT_TABS 3
#define OSK_CHAT_UTILS 3
#define OSK_CHAT_GRID_ROW_FIRST 1
#define OSK_CHAT_GRID_ROW_LAST 3
#define OSK_CHAT_ROW_TABS 0
#define OSK_CHAT_ROW_UTILITY 4
enum {
	OSK_CHAT_TAB_LETTERS = 0,
	OSK_CHAT_TAB_NUMBERS,
	OSK_CHAT_TAB_SYMBOLS,
};
enum {
	OSK_CHAT_UTIL_CASE = 0,
	OSK_CHAT_UTIL_SPACE,
	OSK_CHAT_UTIL_BACK,
};

/* Chat-only view state: where the 640-wide composition sits in the raster and
 * the case the Letters tab remembers. SPEC: the case target "retains case
 * across tabs; disabled on Numbers/Symbols" and "Letters restores remembered
 * case", so the case bit cannot live in `page` alone.
 *
 * B1b adds the two pieces of chat-only input state. Both are CHAT-SCOPED: the
 * sign editor and every other osk_sign_* caller stay edge-only and un-re-armed,
 * which tests/test_chat_keyboard.c proves. */
struct osk_chat_view {
	int ox;	   /* 0 at 640 wide, 81 at the Wii's 802-wide 16:9 GUI */
	bool upper; /* remembered Letters case */
	/* Held-direction repeat. SPEC "Motion": "Direction edge moves immediately;
	 * first held repeat at 250 ms, subsequent repeats 80 ms, release/
	 * disconnect/page change resets the timer", and "Never accumulate a burst
	 * of repeats after a slow frame". Same shape as gui_util's
	 * gutil_menu_repeat_step, with SPEC's cadence instead of the shared menus'
	 * 350/100 -- parameterising the shared helper would need gui_util.c and
	 * would retune every other menu. */
	struct gutil_menu_repeat repeat;
	/* Neutral re-arm (SPEC: "require neutral rearming before fresh actions").
	 * One bit per OSK_CHAT_ARM_*: set = that button was already down when the
	 * composer opened, when the owner reconnected or when the controller mode
	 * flipped, so its press edge is ignored until it has been seen neutral. */
	unsigned rearm_mask;
	/* Zero-initialised views behave as connected, so a plain {0} view needs no
	 * priming: only a real connected->disconnected->connected transition
	 * re-blocks. */
	bool owner_was_disconnected;
};

/* Buttons the composer re-arms. The mapping to enum input_button lives in
 * widget_osk.c so this header keeps its small dependency set. */
#define OSK_CHAT_ARM_LEFT 0
#define OSK_CHAT_ARM_RIGHT 1
#define OSK_CHAT_ARM_UP 2
#define OSK_CHAT_ARM_DOWN 3
#define OSK_CHAT_ARM_CLICK 4
#define OSK_CHAT_ARM_BACK 5
#define OSK_CHAT_ARM_PAGE 6
#define OSK_CHAT_ARM_PREVIOUS 7
#define OSK_CHAT_ARM_SEND 8
#define OSK_CHAT_ARM_CANCEL 9
#define OSK_CHAT_ARM_HOME 10
/* The OPENER. On PC IB_CHAT defaults to the 'T' key, and screen_chat's reset
 * drains the character queue once and then arms the GLFW char callback -- so
 * GLFW key-REPEAT characters from the still-held T arrive AFTER that drain and
 * land in the draft. While this bit is set (IB_CHAT has not been seen up since
 * the composer opened) the chat text queue is drained and DISCARDED, which is
 * what makes "the press that opened chat never types" true on PC too. */
#define OSK_CHAT_ARM_CHAT 11
#define OSK_CHAT_ARM_COUNT 12

/* SPEC "Motion", in seconds. */
#define OSK_CHAT_REPEAT_DELAY_S 0.25F
#define OSK_CHAT_REPEAT_RATE_S 0.08F

/* What one chat update did, so the screen can drive the SPEC's timed states
 * (pressed 80 ms, cap flash 120 ms) without re-deriving them from the buffer. */
struct osk_chat_result {
	enum osk_event event;
	bool text_queued; /* the PC text queue produced a character this frame */
	bool activated;	  /* a key was pressed this frame (pressed feedback) */
	bool rejected;	  /* a printable insertion was refused at the cap */
	int press_row, press_col; /* the pressed key, or -1/-1 */
	/* B1b focus cue. SPEC: FOCUS fires "only on the first actual move of a
	 * fresh directional press or a new pointer target, never repeated held
	 * steps", and never when the cursor did not actually move. */
	bool moved;			/* row/col really changed this frame */
	bool moved_repeat;	/* ... and it came from a held repeat step */
	bool page_changed;	/* a tab/page action landed (resets the repeat timer) */
	/* The repeat countdown AS THE STEP DECISION SAW IT this frame: after the
	 * frame's decrement and BEFORE any reload. `chat_repeat_step` steps exactly
	 * when this has run out, so a stepping frame reports <= 0 and a waiting
	 * frame reports what is left -- which is why it cannot be re-derived from
	 * `view->repeat.remaining` afterwards (a step reloads that to the 80 ms
	 * rate, a page change to the 250 ms delay). The scripted mp_chat_login_3m
	 * trace publishes this value rather than a constant, so "never a step while
	 * the countdown is positive" is checked against production's own number
	 * (docs/chat_login_profile.md section 3). Purely reported: nothing in the
	 * widget reads it back. */
	float repeat_seen_s;
};

/* Tab <-> page mapping. Letters resolves to LOWER or UPPER through `upper`;
 * Numbers is OSK_PAGE_NUM and Symbols is OSK_PAGE_SYMBOLS -- the existing
 * tables, in the existing order (SPEC "Keyboard grid and page states"). */
int osk_chat_tab_of_page(int page);
int osk_chat_page_of_tab(int tab, bool upper);
/* "Case label ABC while lower, abc while upper ... disabled on Numbers/Symbols" */
bool osk_chat_case_enabled(int page);
const char* osk_chat_tab_label(int tab);
const char* osk_chat_case_label(bool upper);

/* Cells present in a row: 3 tabs, the page's characters (10/10/10 on Letters,
 * 10/10/1 on Numbers and Symbols), 3 utilities. 0 = an empty row. */
int osk_chat_row_count(int page, int row);
/* Can the cursor land here? Absent cells and the disabled case key cannot. */
bool osk_chat_selectable(int page, int row, int col);

/* The SPEC key rects, offset by `ox`: tabs 172x32 at 56/234/412 y204; the
 * 49/50x40 character cells (left = 56 + 4c + floor(492c/10)) on rows y244/288/
 * 332; the 84/220/216x40 utilities at 56/144/368 y380. */
bool osk_chat_key_bounds(int ox, int row, int col,
						 struct gutil_button_bounds* out);

/* Open the composer's input state: every currently-held chat button is blocked
 * until it has been seen neutral, and the repeat timer is cleared. Call from
 * the screen's reset (B1b input hygiene: "the press that OPENED chat never
 * types or activates a key"). */
void osk_chat_input_reset(struct osk_chat_view* view);

/* Advance the re-arm once per frame: unblock every chat button that is now
 * neutral, and re-block all of them across an owner disconnect. The screen
 * calls this before reading Send/Cancel; osk_chat_update_owned calls it too
 * and the two calls are idempotent. */
void osk_chat_input_tick(struct osk_chat_view* view,
						 const struct input_player_frame* owner);

/* One frame of chat keyboard input. Same shape as osk_sign_update_owned (the
 * pointer overrides the cursor, B backspaces from anywhere, the PC text queue
 * drains through osk_feed_char), plus the tab row, the utility row, the
 * existing IB_GUI_PREVIOUS / IB_GUI_PAGE page actions, held-direction repeat
 * and the neutral re-arm. `dt` is the frame time in seconds. */
void osk_chat_update_owned(struct osk_state* o, struct osk_chat_view* view,
						   float dt, bool ptr, float px, float py,
						   bool allow_click,
						   const struct input_player_frame* owner,
						   bool keyboard, struct osk_chat_result* out);

/* The Send and Cancel press edges, owner-gated and neutral-re-armed exactly
 * like the keyboard's own buttons, so the screen has one hygiene rule rather
 * than two. Cancel is IB_INVENTORY or IB_HOME, as today. Call at most once per
 * frame: they consume the re-arm state. */
bool osk_chat_send_edge(struct osk_chat_view* view,
						const struct input_player_frame* owner);
bool osk_chat_cancel_edge(struct osk_chat_view* view,
						  const struct input_player_frame* owner);
/* The A and B edges, re-armed the same way. The screen needs them to decide
 * whether a POINTER click lands on its own Send/Cancel targets, and reading
 * them raw from the owner frame would let the press that opened the composer
 * drive those targets. Querying is non-destructive: the widget consumes the
 * same edges itself. */
bool osk_chat_click_edge(struct osk_chat_view* view,
						 const struct input_player_frame* owner);
bool osk_chat_back_edge(struct osk_chat_view* view,
						const struct input_player_frame* owner);

/* Presentation constants the chat SCREEN draws with. They live here beside the
 * key rects they decorate, but the drawing itself is screen_chat.c's: this
 * widget must not acquire a dependency on gutil_prompt_glyph /
 * gutil_menu_focus_border, because the frozen sign test links widget_osk.c on
 * its own with a minimal set of boundary doubles.
 *
 * "Previous-page glyph (24 pixels) is at (60,208); next-page glyph at
 * (556,208). These live inside the first/last tabs as shortcut hints, NOT extra
 * click targets"; "Active tab has a 2-pixel white underline inset 8 pixels, at
 * y232"; and the widget-sheet rows from "Art, typography and states". */
#define OSK_CHAT_PAGE_HINT_Y 208
#define OSK_CHAT_PREV_HINT_X 60
#define OSK_CHAT_NEXT_HINT_X 556
#define OSK_CHAT_TAB_UNDERLINE_Y 232
#define OSK_CHAT_TAB_UNDERLINE_INSET 8
#define OSK_CHAT_TAB_UNDERLINE_H 2
#define OSK_CHAT_TEX_DISABLED 46
#define OSK_CHAT_TEX_NORMAL 66
#define OSK_CHAT_TEX_SELECTED 86
#define OSK_CHAT_BORDER 2

/* Approved sign profile; other text editors retain the legacy keyboard. */
struct gutil_button_bounds;
void osk_sign_reset(struct osk_state*,char*,size_t,int);
char osk_sign_page_char(int page,int row,int col);
bool osk_sign_key_bounds(int x,int y,int width,bool compact,int row,int col,struct gutil_button_bounds*);
enum osk_event osk_sign_update(struct osk_state*,int x,int y,int width,bool compact,bool pointer,float px,float py,bool allow_click,bool* text_queued);
struct input_player_frame;
enum osk_event osk_sign_update_owned(struct osk_state*,int,int,int,bool,bool,float,float,bool,bool*,const struct input_player_frame*,bool keyboard);
void osk_sign_render(const struct osk_state*,int x,int y,int width,bool compact);

/* Opt-in FRONTEND editor only (approved front-end-refined/editor.js).
 * Existing append-only, sign and chat profiles remain unchanged. Mid-string
 * editing is an explicit frontend/controller adaptation; limits and events
 * retain the legacy contract. No range selection is advertised or simulated. */
enum osk_frontend_target {
	OSK_FRONT_PREV, OSK_FRONT_LETTERS, OSK_FRONT_NUMBERS, OSK_FRONT_SYMBOLS,
	OSK_FRONT_NEXT, OSK_FRONT_CHAR_FIRST,
	OSK_FRONT_CASE = OSK_FRONT_CHAR_FIRST + 30,
	OSK_FRONT_SPACE, OSK_FRONT_LEFT, OSK_FRONT_RIGHT, OSK_FRONT_BACK,
	OSK_FRONT_CANCEL, OSK_FRONT_DONE,
	/* Create-only targets (T05a), appended so no existing ordinal moves. They
	 * have no bounds and are disabled in OSK_FRONT_PROFILE_DEFAULT. */
	OSK_FRONT_RANDOM, OSK_FRONT_DIGIT_FIRST, /* name grid's digit row, 10 */
	OSK_FRONT_TARGET_COUNT = OSK_FRONT_DIGIT_FIRST + 10
};
enum osk_frontend_profile {
	OSK_FRONT_PROFILE_DEFAULT,     /* every existing screen, unchanged */
	OSK_FRONT_PROFILE_CREATE_NAME, /* opus-B name OSK: digit row + letters */
	OSK_FRONT_PROFILE_CREATE_SEED  /* opus-B seed keypad: 1-9 - 0 abc Random */
};
struct osk_frontend {
	struct osk_state edit;
	size_t caret, reveal_start;
	int layout, target, origin;
	bool upper, address, input_seen, was_connected;
	int category;
	unsigned blocked, previous_down;
	int prompt_glyphs[5]; /* Previous, Next, Backspace, Cancel, Done; original art */
	struct gutil_menu_focus pointer;
	int profile; /* enum osk_frontend_profile; reset() sets DEFAULT */
};
/* Create World only: choose the opus-B geometry; sets layout/target. */
void osk_frontend_set_profile(struct osk_frontend*, int profile);
void osk_frontend_reset(struct osk_frontend*, char*, size_t, int beta_max,
	int layout, bool address);
/* Rebind after the parent changes fields; preserves layout and focus while
 * placing the insertion point at the new field's end. */
void osk_frontend_bind(struct osk_frontend*, char*, size_t, int beta_max);
/* Preserve an assigned host, including disconnection. Only an unbound PC
 * uses the ordinary keyboard/mouse mask in scratch. */
const struct input_player_frame* osk_frontend_input_frame(
	struct input_player_frame* scratch);
enum osk_event osk_frontend_feed_char(struct osk_frontend*, char);
char osk_frontend_char(const struct osk_frontend*, int index);
bool osk_frontend_enabled(const struct osk_frontend*, int target, bool done);
bool osk_frontend_key_bounds(const struct osk_frontend*, int width, int height,
	int target, struct gutil_button_bounds*);
enum osk_event osk_frontend_update_owned(struct osk_frontend*, int width,
	int height, const struct input_player_frame*, bool keyboard_allowed,
	bool pointer, float px, float py, bool allow_click, bool done_enabled);
/* Includes Cancel/Done targets, which emit events and never act on the parent. */
void osk_frontend_render(const struct osk_frontend*, int width, int height,
	bool done_enabled);
/* The keyboard's centred hint row (10-31): Type / Done / Delete / Keys /
 * Cancel from `owner`'s bound glyphs, between the Cancel and Done keys. */
void osk_frontend_hint_row(const struct osk_frontend*,
	const struct input_player_frame* owner, int width, int height);
/* Field renderer measures/renders from this byte index; keep room for a caret.
 * This function never changes text or insertion position. */
size_t osk_frontend_reveal(struct osk_frontend*, int font_size, int room);
#endif
