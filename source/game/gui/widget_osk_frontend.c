/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "../../platform/input_owner.h"
#include "../../platform/input_glyph.h"
#include "widget_osk.h"
#include "../menu_autopilot.h"

/* Frontend-only editor profile. Approved editor.js/refinement.css; caller owns
 * validation and completion. Legacy/sign/chat implementations remain untouched. */
#include "frontend_presentation.h"

/* A synthesized player-1 frame from the merged action mask: input_pressed /
 * input_held, which is where the menus_dwell walker's presses arrive
 * (menu_autopilot_force_button). */
static const struct input_player_frame* osk_frontend_merged_frame(
	struct input_player_frame* scratch) {
	input_prompt_frame(0, scratch);
	scratch->connected = true;
	for(int b = 0; b < IB_COUNT; b++) {
		scratch->pressed[b] = input_pressed((enum input_button)b);
		scratch->down[b] = input_held((enum input_button)b);
	}
	return scratch;
}

const struct input_player_frame* osk_frontend_input_frame(
	struct input_player_frame* scratch) {
	/* menus_dwell (menus-dwell-walker-1026, hw1026c): while the walker is
	 * ARMED it owns player 1's input for its whole window, exactly as it owns
	 * the pointer (menu_autopilot_force_pointer). Its presses reach
	 * input_pressed(), never an owner frame, and the rig claims no player-1
	 * controller -- so without this every frontend editor and parent form
	 * (Create World's name modal, Rename World, Multiplayer Direct/Add/Edit/
	 * player name) was deaf to it and the no-pointer leg died at
	 * STEP_NP_SUB_WAIT_OUT inside Direct Connect. menu_autopilot_active() is
	 * false in every non-hw_run boot, so ordinary play is untouched; this is
	 * the input twin of input_prompt_frame's walker presentation frame. */
	if(menu_autopilot_active())
		return osk_frontend_merged_frame(scratch);
	const struct input_player_frame* owner = input_player_frame(0);
#ifdef PLATFORM_PC
	if(!owner)
		owner = osk_frontend_merged_frame(scratch);
#else
	(void)scratch;
#endif
	return owner;
}

static int front_scale(int n, int height) {
	return (int)((float)n * (height > 0 ? height : 480) / 480.0F + .5F);
}
static int front_offset(int width, int height) {
	int x = (width - front_scale(640, height)) / 2;
	return x > 0 ? x : 0;
}
/* ---- Create World profiles (ui-build-1026 T05a; design-round-1026c opus-B
 * name-osk / seed-osk). Opt-in through osk_frontend_set_profile(); with
 * OSK_FRONT_PROFILE_DEFAULT every function below takes its original path, so
 * Rename World and the Multiplayer editors keep their exact layout. The name
 * grid is the letters/symbols page plus a fixed digit row; the seed opens on a
 * phone keypad (digits, '-', Random) whose "abc" key reaches the same grid,
 * because Beta accepts any text as a seed (String.hashCode, 740c583
 * GuiCreateWorld.actionPerformed l.75-86, resolved by screen_create_world.c).
 * Caret keys are not offered: Create edits at the end, as Beta's field does
 * with a pad. */
static bool front_create(const struct osk_frontend* o) {
	return o->profile == OSK_FRONT_PROFILE_CREATE_NAME
		|| o->profile == OSK_FRONT_PROFILE_CREATE_SEED;
}
static bool front_keypad(const struct osk_frontend* o) {
	return o->profile == OSK_FRONT_PROFILE_CREATE_SEED && o->layout == 1;
}
/* Character index of a key target: 0..29 page cells, 30..39 digit row. */
static int front_char_index(int t) {
	if(t >= OSK_FRONT_CHAR_FIRST && t < OSK_FRONT_CASE)
		return t - OSK_FRONT_CHAR_FIRST;
	if(t >= OSK_FRONT_DIGIT_FIRST && t < OSK_FRONT_DIGIT_FIRST + 10)
		return 30 + t - OSK_FRONT_DIGIT_FIRST;
	return -1;
}
/* The page-switch key this layout shows (same cell on every page). */
static int front_mode_key(const struct osk_frontend* o) {
	if(o->layout != 0) return OSK_FRONT_LETTERS;
	return o->profile == OSK_FRONT_PROFILE_CREATE_SEED
		? OSK_FRONT_NUMBERS : OSK_FRONT_SYMBOLS;
}
/* 640x480 rectangles of the Create profiles, under the parent's field
 * (y 76-128) and seed hint (y 136-150); the Type hint (y 412) sits between
 * Cancel and the right edge. Keys: name grid 54x44, keypad 180x44. */
static bool front_create_rect(const struct osk_frontend* o, int t,
		int* x, int* y, int* w) {
	if(t == OSK_FRONT_CANCEL) { *x = 32; *y = 408; *w = 180; return true; }
	int ci = front_char_index(t);
	if(front_keypad(o)) {
		int cell = -1;
		if(ci >= 0) cell = osk_frontend_char(o, ci) ? ci : -1;
		else if(t == OSK_FRONT_LETTERS) cell = 11;
		else if(t == OSK_FRONT_RANDOM) cell = 12;
		else if(t == OSK_FRONT_BACK) cell = 13;
		else if(t == OSK_FRONT_DONE) cell = 14;
		if(cell < 0) return false;
		*x = 42 + (cell % 3) * 188; *y = 154 + (cell / 3) * 48; *w = 180;
		return true;
	}
	if(ci >= 0) {
		if(!osk_frontend_char(o, ci)) return false;
		int row = ci >= 30 ? 0 : 1 + ci / 10, col = ci >= 30 ? ci - 30 : ci % 10;
		*x = 32 + col * 58; *y = 154 + row * 48; *w = 54;
		return true;
	}
	*y = 346;
	if(t == OSK_FRONT_CASE && o->layout == 0) { *x = 32; *w = 80; return true; }
	if(t == front_mode_key(o)) { *x = 116; *w = 80; return true; }
	if(t == OSK_FRONT_SPACE) { *x = 200; *w = 180; return true; }
	if(t == OSK_FRONT_BACK) { *x = 384; *w = 110; return true; }
	if(t == OSK_FRONT_DONE) { *x = 498; *w = 110; return true; }
	return false;
}
void osk_frontend_set_profile(struct osk_frontend* o, int profile) {
	assert(o && profile >= OSK_FRONT_PROFILE_DEFAULT
		&& profile <= OSK_FRONT_PROFILE_CREATE_SEED);
	o->profile = profile;
	if(!front_create(o)) return;
	o->layout = profile == OSK_FRONT_PROFILE_CREATE_SEED ? 1 : 0;
	o->upper = false;
	o->target = OSK_FRONT_CHAR_FIRST;
}
void osk_frontend_bind(struct osk_frontend* o, char* buf, size_t cap, int max) {
	assert(o && buf && cap && memchr(buf, 0, cap));
	osk_reset(&o->edit, buf, cap, max);
	o->caret = strlen(buf);
	o->reveal_start = 0;
}
void osk_frontend_reset(struct osk_frontend* o, char* buf, size_t cap, int max,
						int layout, bool address) {
	assert(o);
	*o = (struct osk_frontend){0};
	for(int i = 0; i < 5; i++) o->prompt_glyphs[i] = -1;
	osk_frontend_bind(o, buf, cap, max);
	o->layout = layout >= 0 && layout <= 2 ? layout : 0;
	o->address = address;
	o->target = OSK_FRONT_CHAR_FIRST;
}
enum osk_event osk_frontend_feed_char(struct osk_frontend* o, char ch) {
	assert(o && o->edit.buf);
	size_t len = strlen(o->edit.buf);
	if(o->caret > len) o->caret = len;
	if(ch == '\b') {
		if(!o->caret) return OSK_EV_NONE;
		memmove(o->edit.buf + o->caret - 1, o->edit.buf + o->caret,
			len - o->caret + 1);
		o->caret--;
		return OSK_EV_CHANGED;
	}
	/* The old mutator remains the authority for printable filtering, bounds
	 * and control events. It appends, then this opted-in profile rotates that
	 * one accepted character into the insertion point without losing a byte. */
	enum osk_event ev = osk_feed_char(&o->edit, ch);
	if(ev == OSK_EV_CHANGED) {
		memmove(o->edit.buf + o->caret + 1, o->edit.buf + o->caret,
			len - o->caret);
		o->edit.buf[o->caret++] = ch;
	}
	return ev;
}
char osk_frontend_char(const struct osk_frontend* o, int index) {
	const char* chars = o->layout == 1
		? (o->address ? "123.456:7890" : "123456789-0+")
		: o->layout == 2 ? "!@#$%^&*()[]{}<>/\\:;\"'?,_=`|~"
		: "qwertyuiopasdfghjkl-zxcvbnm.,/";
	/* Native ` | ~ replace duplicate . + - (available on Letters/Numbers)
	 * from the browser sample, preserving ASCII coverage within thirty cells. */
	if(front_create(o)) {
		/* Digit row on the grid pages; the keypad stops at '0' (index 10),
		 * dropping "+" which Beta's parseLong rejects anyway. */
		if(index >= 30 && index < 40 && o->layout != 1)
			return "1234567890"[index - 30];
		if(front_keypad(o) && index > 10) return 0;
	}
	if(index < 0 || (size_t)index >= strlen(chars)) return 0;
	char ch = chars[index];
	return o->layout == 0 && o->upper && ch >= 'a' && ch <= 'z'
		? (char)(ch - 'a' + 'A') : ch;
}
bool osk_frontend_enabled(const struct osk_frontend* o, int t, bool done) {
	if(!o || t < 0 || t >= OSK_FRONT_TARGET_COUNT) return false;
	if(front_create(o)) {
		int x, y, w;
		return front_create_rect(o, t, &x, &y, &w)
			&& (t != OSK_FRONT_DONE || done);
	}
	if(t >= OSK_FRONT_RANDOM) return false;
	if(t >= OSK_FRONT_CHAR_FIRST && t < OSK_FRONT_CASE)
		return osk_frontend_char(o, t - OSK_FRONT_CHAR_FIRST) != 0;
	if(t == OSK_FRONT_PREV) return o->layout > 0;
	if(t == OSK_FRONT_NEXT) return o->layout < 2;
	if(t == OSK_FRONT_CASE) return o->layout == 0;
	if(t == OSK_FRONT_SPACE) return !o->address;
	if(t == OSK_FRONT_LEFT) return o->caret > 0;
	if(t == OSK_FRONT_RIGHT) return o->caret < strlen(o->edit.buf);
	if(t == OSK_FRONT_DONE) return done;
	return true;
}
bool osk_frontend_key_bounds(const struct osk_frontend* o, int width,
		int height, int t, struct gutil_button_bounds* out) {
	if(!o || !out || t < 0 || t >= OSK_FRONT_TARGET_COUNT) return false;
	int x, y, w, h = 44;
	if(front_create(o)) {
		if(!front_create_rect(o, t, &x, &y, &w)) return false;
	} else if(t >= OSK_FRONT_RANDOM) {
		return false;
	} else if(t < OSK_FRONT_CHAR_FIRST) {
		y = 156;
		if(t == OSK_FRONT_PREV) { x = 32; w = 44; }
		else if(t == OSK_FRONT_NEXT) { x = 564; w = 44; }
		else {
			int i = t - OSK_FRONT_LETTERS;
			x = 80 + (484 * i + 1) / 3;
			w = 80 + (484 * (i + 1) + 1) / 3 - 4 - x;
		}
	} else if(t < OSK_FRONT_CASE) {
		int n = t - OSK_FRONT_CHAR_FIRST;
		if(!osk_frontend_char(o, n)) return false;
		int cols = o->layout == 1 ? 4 : 10;
		w = o->layout == 1 ? 92 : 54;
		x = (o->layout == 1 ? 130 : 32) + (n % cols) * (w + 4);
		y = 208 + (n / cols) * 48;
	} else if(t <= OSK_FRONT_BACK) {
		static const int xs[] = {32, 100, 280, 328, 376};
		static const int ws[] = {64, 176, 44, 44, 232};
		x = xs[t - OSK_FRONT_CASE]; w = ws[t - OSK_FRONT_CASE]; y = 356;
	} else {
		/* Cancel / Done: 120 wide (were 180) so the centred hint row
		 * (10-31) fits between them on the same 408-452 band. */
		x = t == OSK_FRONT_CANCEL ? 32 : 488; y = 408; w = 120;
	}
	int ox = front_offset(width, height);
	*out = (struct gutil_button_bounds){ox + front_scale(x, height),
		front_scale(y, height), front_scale(x + w, height) - front_scale(x, height),
		front_scale(y + h, height) - front_scale(y, height)};
	return true;
}
static void front_layout(struct osk_frontend* o, int page) {
	/* The name profile has no separate numbers page (its digit row is
	 * always shown): paging steps straight between letters and symbols. */
	if(o->profile == OSK_FRONT_PROFILE_CREATE_NAME && page == 1)
		page = o->layout == 0 ? 2 : 0;
	if(page < 0 || page > 2) return;
	o->layout = page;
	if(!osk_frontend_enabled(o, o->target, true))
		o->target = OSK_FRONT_CHAR_FIRST;
}
static enum osk_event front_activate(struct osk_frontend* o, int t) {
	if(t >= OSK_FRONT_CHAR_FIRST && t < OSK_FRONT_CASE)
		return osk_frontend_feed_char(o, osk_frontend_char(o, t - OSK_FRONT_CHAR_FIRST));
	if(front_create(o)) {
		int ci = front_char_index(t);
		if(ci >= 0) return osk_frontend_feed_char(o, osk_frontend_char(o, ci));
		if(t == OSK_FRONT_RANDOM) return OSK_EV_RANDOM;
		if(t >= OSK_FRONT_LETTERS && t <= OSK_FRONT_SYMBOLS) {
			/* The page key keeps the focus: it is the same cell on the
			 * page it opens, so A twice returns where you were. */
			front_layout(o, t - OSK_FRONT_LETTERS);
			o->target = front_mode_key(o);
			return OSK_EV_NONE;
		}
	}
	switch(t) {
		case OSK_FRONT_PREV: front_layout(o, o->layout - 1); break;
		case OSK_FRONT_NEXT: front_layout(o, o->layout + 1); break;
		case OSK_FRONT_LETTERS: case OSK_FRONT_NUMBERS: case OSK_FRONT_SYMBOLS:
			front_layout(o, t - OSK_FRONT_LETTERS); break;
		case OSK_FRONT_CASE: o->upper = !o->upper; break;
		case OSK_FRONT_SPACE: return osk_frontend_feed_char(o, ' ');
		case OSK_FRONT_LEFT: if(o->caret) o->caret--; break;
		case OSK_FRONT_RIGHT: if(o->caret < strlen(o->edit.buf)) o->caret++; break;
		case OSK_FRONT_BACK: return osk_frontend_feed_char(o, '\b');
		case OSK_FRONT_CANCEL: return OSK_EV_CANCEL;
		case OSK_FRONT_DONE: return OSK_EV_DONE;
		default: break;
	}
	return OSK_EV_NONE;
}
/* Directional neighbours come from the same rectangles as the pointer and
 * draw path. Vertical moves prefer the closest row, then closest centre;
 * horizontal moves stay on their row. At outer edges focus clamps. */
static void front_move(struct osk_frontend* o, int dx, int dy, bool done) {
	struct gutil_button_bounds here;
	if(!osk_frontend_key_bounds(o, 640, 480, o->target, &here)) {
		o->target = OSK_FRONT_CHAR_FIRST; return;
	}
	int best = -1, score = 0x7FFFFFFF;
	for(int t = 0; t < OSK_FRONT_TARGET_COUNT; t++) {
		struct gutil_button_bounds r;
		if(t == o->target || !osk_frontend_enabled(o, t, done)
		   || !osk_frontend_key_bounds(o, 640, 480, t, &r)) continue;
		int vx = (r.x * 2 + r.width) - (here.x * 2 + here.width);
		int vy = r.y - here.y;
		if((dx && (vy || vx * dx <= 0)) || (dy && vy * dy <= 0)) continue;
		int cost = dy ? abs(vy) * 2048 + abs(vx) : abs(vx);
		if(cost < score) { score = cost; best = t; }
	}
	if(best >= 0) o->target = best;
}
/* Only these roles belong to the opted-in frontend editor. Physical keyboard
 * typing must not alias a WASD/Space/Enter binding into a second UI action. */
static const enum input_button front_roles[] = {
	IB_GUI_LEFT, IB_GUI_RIGHT, IB_GUI_UP, IB_GUI_DOWN, IB_GUI_CLICK,
	IB_GUI_CLICK_ALT, IB_GUI_PREVIOUS, IB_GUI_PAGE, IB_GUI_QUICK_MOVE,
	IB_INVENTORY, IB_HOME
};
enum osk_event osk_frontend_update_owned(struct osk_frontend* o, int width,
		int height, const struct input_player_frame* owner, bool keyboard,
		bool ptr, float px, float py, bool allow_click, bool done) {
	assert(o && o->edit.buf);
	bool connected = owner && owner->connected;
	bool reconnect = o->input_seen && (!o->was_connected
		|| (connected && o->category != (int)owner->category));
	/* GLFW keeps collecting text while an assigned owner is absent. Discard
	 * that interval, including events arriving just before its reconnect frame,
	 * so stale Enter/Escape cannot commit or cancel the retained draft. */
	if(keyboard && (!connected || reconnect)) {
		char discard;
		while(input_text_poll(&discard)) {}
	}
	if(!connected) {
		o->input_seen = true;
		o->was_connected = false; o->previous_down = 0; return OSK_EV_NONE;
	}
	unsigned edges = 0, down = 0;
	for(size_t i = 0; i < sizeof(front_roles) / sizeof(front_roles[0]); i++) {
		unsigned bit = 1u << i;
		bool held = owner->down[front_roles[i]];
		if(held) down |= bit;
		if((!o->input_seen || reconnect) && held) o->blocked |= bit;
		if(!held) o->blocked &= ~bit;
		if(owner->pressed[front_roles[i]] && !(o->blocked & bit)
		   && !(o->previous_down & bit)) edges |= bit;
	}
	o->input_seen = o->was_connected = true;
	o->category = owner->category; o->previous_down = down;
	static const enum input_button prompts[] = {IB_GUI_PREVIOUS, IB_GUI_PAGE,
		IB_GUI_CLICK_ALT, IB_INVENTORY, IB_GUI_QUICK_MOVE};
	for(int i = 0; i < 5; i++) o->prompt_glyphs[i] = owner->glyphs[prompts[i]];
	bool typing = keyboard && input_text_active();
	bool queued = false;
	enum osk_event result = OSK_EV_NONE;
	char ch;
	while(keyboard && input_text_poll(&ch)) {
		queued = true;
		enum osk_event ev = osk_frontend_feed_char(o, ch);
		if(ev == OSK_EV_DONE && !done) continue;
		if(ev != OSK_EV_NONE) result = ev;
		/* A terminal event ends this frame; the parent changes the field/screen
		 * before any later queued character is allowed to mutate another draft. */
		if(ev == OSK_EV_DONE || ev == OSK_EV_CANCEL || ev == OSK_EV_NEXT)
			return ev;
	}
	if(typing) {
		edges = 0;
		if(input_text_mouse_pressed(IB_GUI_CLICK)) edges |= 1u << 4;
		if(input_text_mouse_pressed(IB_GUI_CLICK_ALT)) edges |= 1u << 5;
	}
	if(queued) return result;
	int dx = (edges & 1u) ? -1 : (edges & 2u) ? 1 : 0;
	int dy = (edges & 4u) ? -1 : (edges & 8u) ? 1 : 0;
	bool nav = dx || dy;
	if(nav) { front_move(o, dx, dy, done); o->origin = GUTIL_MENU_ORIGIN_DIRECTIONAL; }
	ptr = ptr && isfinite(px) && isfinite(py);
	int hover = GUTIL_MENU_TARGET_NONE;
	for(int t = 0; ptr && t < OSK_FRONT_TARGET_COUNT; t++) {
		struct gutil_button_bounds r;
		if(osk_frontend_key_bounds(o, width, height, t, &r) && gutil_button_hit(&r, px, py)) {
			hover = t; break;
		}
	}
	bool pointer_owner = gutil_menu_pointer_focus(&o->pointer, ptr, px, py, nav);
	if(pointer_owner && hover >= 0 && osk_frontend_enabled(o, hover, done)) {
		o->target = hover; o->origin = GUTIL_MENU_ORIGIN_POINTER;
	}
	if(nav) return OSK_EV_NONE;
	/* Canonical R-C: an activate edge is consumed even if it did nothing.
	 * A remapped shared physical button cannot also Cancel the draft. */
	if(edges & (1u << 4)) {
		struct gutil_menu_decision d = gutil_menu_activate_decide((struct gutil_menu_activate_state){
		/* The canonical origin rule keeps pointer-owned background inert,
		 * while directional focus remains usable with valid IR off a key. */
		.pressed = allow_click, .navigated = nav,
		.pointer_valid = ptr, .selection_origin = (enum gutil_menu_origin)o->origin,
		.hovered = hover, .hovered_enabled = osk_frontend_enabled(o, hover, done),
		.selected = o->target, .selected_enabled = osk_frontend_enabled(o, o->target, done),
		.selected_destructive = o->target == OSK_FRONT_CANCEL
		});
		if(d.action == GUTIL_MENU_FOCUS_ONLY) o->target = d.target;
		o->origin = d.selection_origin;
		return d.action == GUTIL_MENU_ACTIVATE ? front_activate(o, d.target) : OSK_EV_NONE;
	}
	if(edges & ((1u << 9) | (1u << 10))) return OSK_EV_CANCEL;
	/* Paging owns a shared physical edge before the optional Done shortcut. */
	if(edges & (1u << 6)) { front_layout(o, o->layout - 1); return OSK_EV_NONE; }
	if(edges & (1u << 7)) { front_layout(o, o->layout + 1); return OSK_EV_NONE; }
	if(edges & (1u << 8)) return done ? OSK_EV_DONE : OSK_EV_NONE;
	if(edges & (1u << 5)) return osk_frontend_feed_char(o, '\b');
	return OSK_EV_NONE;
}
size_t osk_frontend_reveal(struct osk_frontend* o, int font, int room) {
	size_t len = strlen(o->edit.buf);
	if(o->caret > len) o->caret = len;
	if(o->reveal_start > o->caret) o->reveal_start = o->caret;
	int width = 0;
	for(size_t i = o->reveal_start; i < o->caret; i++) {
		char ch[2] = {o->edit.buf[i], 0}; width += gutil_font_width(ch, font);
	}
	while(width > room && o->reveal_start < o->caret) {
		char ch[2] = {o->edit.buf[o->reveal_start++], 0};
		width -= gutil_font_width(ch, font);
	}
	return o->reveal_start;
}
void osk_frontend_render(const struct osk_frontend* o, int width, int height, bool done) {
	for(int t = 0; t < OSK_FRONT_TARGET_COUNT; t++) {
		struct gutil_button_bounds b;
		if(!osk_frontend_key_bounds(o, width, height, t, &b)) continue;
		bool enabled = osk_frontend_enabled(o, t, done), hot = enabled && t == o->target;
		struct fep_rect r = fep_rect(b.x, b.y, b.width, b.height);
		bool create = front_create(o), keypad = front_keypad(o);
		int ci = front_char_index(t);
		bool tab = !create && t >= OSK_FRONT_LETTERS && t <= OSK_FRONT_SYMBOLS;
		bool pager = t == OSK_FRONT_PREV || t == OSK_FRONT_NEXT;
		if(!pager) {
			if(tab && !hot) fep_paper(r); else fep_widget(r, hot);
		}
		if(tab && t - OSK_FRONT_LETTERS == o->layout)
			fep_fill(fep_rect(r.x + front_scale(8,height), r.y + r.h - front_scale(3,height),
				r.w - front_scale(16,height), front_scale(3,height)), 0x35,0x35,0x35,255);
		if(hot) fep_outline(r, front_scale(2,height),255,255,160,255);
		if(!enabled && !pager) fep_fill(r,0,0,0,100);
		char one[2] = {0}; const char* label = one;
		if(ci >= 0) one[0] = osk_frontend_char(o,ci);
		else if(create && t >= OSK_FRONT_LETTERS && t <= OSK_FRONT_SYMBOLS)
			label = t == OSK_FRONT_LETTERS ? "abc" : t == OSK_FRONT_NUMBERS ? "123" : "!#?";
		else if(create && t == OSK_FRONT_CASE) label = o->upper ? "SHIFT" : "Shift";
		else if(create && t == OSK_FRONT_BACK) label = "Delete";
		else if(t == OSK_FRONT_RANDOM) label = "Random";
		else switch(t) {
			case OSK_FRONT_PREV: label = "<"; break;
			case OSK_FRONT_NEXT: label = ">"; break;
			case OSK_FRONT_LETTERS: label = "Letters"; break;
			case OSK_FRONT_NUMBERS: label = "Numbers"; break;
			case OSK_FRONT_SYMBOLS: label = "Symbols"; break;
			case OSK_FRONT_CASE: label = o->upper ? "abc" : "ABC"; break;
			case OSK_FRONT_SPACE: label = "Space"; break;
			case OSK_FRONT_LEFT: label = "<"; break;
			case OSK_FRONT_RIGHT: label = ">"; break;
			case OSK_FRONT_BACK: label = "Backspace"; break;
			case OSK_FRONT_CANCEL: label = "Cancel"; break;
			case OSK_FRONT_DONE: label = "Done"; break;
			default: break;
		}
		int font = front_scale(ci >= 0 ? (create ? (keypad ? 26 : 24) : 22) : 16,height);
		int ink = (tab || pager) && !hot ? 0x35 : enabled ? 0xF4 : 0xA0;
		int prompt = t == OSK_FRONT_PREV ? 0 : t == OSK_FRONT_NEXT ? 1
			: t == OSK_FRONT_BACK ? 2 : t == OSK_FRONT_CANCEL ? 3
			: t == OSK_FRONT_DONE ? 4 : -1;
		/* No glyph on a key (controller UI redesign 10-31, Q6): the pager
		 * keys are plain "<" ">" click targets and the key roles are named
		 * in osk_frontend_hint_row. */
		(void)prompt;
		int label_width = gutil_font_width(label,font);
		int text_x = r.x + (r.w - label_width)/2;
		gutil_text_rgb(text_x,r.y+(r.h-font)/2,
			label,font,!tab,hot?255:ink,hot?255:ink,hot?160:ink);
	}
}

/* The keyboard's hint row (controller UI redesign 10-31, Q6): the key-glyph
 * prompts that used to sit on Cancel / Done / Backspace / the pager, plus the
 * parent's "Type", as ONE centred A1 row. The 408-452 band is shared with the
 * Cancel (and default-profile Done) key, so the row is centred in the free
 * span between them: from Cancel's right edge to Done's left edge, or to the
 * canvas's right edge when Done is not on that band (Create profiles). When
 * it overflows, A1 drops the page pair first, then Delete; Cancel is kept. */
void osk_frontend_hint_row(const struct osk_frontend* o,
		const struct input_player_frame* owner, int width, int height) {
	if(!o || !owner) return;
	struct gutil_button_bounds cancel = {0}, done = {0};
	int left = front_offset(width, height) + front_scale(32, height);
	int right = front_offset(width, height) + front_scale(608, height);
	if(osk_frontend_key_bounds(o, width, height, OSK_FRONT_CANCEL, &cancel))
		left = cancel.x + cancel.width;
	if(osk_frontend_key_bounds(o, width, height, OSK_FRONT_DONE, &done)
	   && done.y == cancel.y && done.x > left)
		right = done.x;
	bool create = front_create(o);
	const struct gutil_hint_item items[] = {
		{{IB_GUI_CLICK}, 1, "Type", 5, false},
		{{IB_GUI_QUICK_MOVE}, 1, "Done", 4, false},
		{{IB_GUI_CLICK_ALT}, 1, create ? "Delete" : "Backspace", 2, false},
		{{IB_GUI_PREVIOUS, IB_GUI_PAGE}, 2, "Keys", 1, false},
		{{IB_INVENTORY}, 1, "Cancel", 0, true}};
	struct gutil_hint_row row;
	if(gutil_hint_row_layout(owner, left, 0, right - left, height, false,
			GUTIL_HINT_ALIGN_CENTRE, items, 5, &row))
		gutil_hint_row_draw(&row);
}
