/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifndef MENU_HEADER_H
#define MENU_HEADER_H

#include <stdbool.h>
#include <stddef.h>

#include "../../graphics/gfx_settings.h"

/* Shared header / controller-notice composition for the native menus.
 *
 * Design authority: docs/ui_design/approved/menus.html (the menu selector's
 * pause / furnace / dispenser / sign editor / death compositions) and
 * docs/ui_design/approved/specs/menus.md, "Controller events": "Notifications
 * fit within their viewport and do not cover menu actions or the other
 * player's HUD."
 *
 * This module RESOLVES the open decision left by the controller handoff
 * (C:/tmp/cw-l3/docs/ui_design/CONTROLLER_HANDOFF.md: "How should ordinary
 * notices coexist with each native menu header? ... Prefer the approved
 * per-menu header composition over masking a fixed-height strip
 * indiscriminately."). The approved prototype answers it in one line of its
 * own render loop:
 *
 *     const inHeader = state.layout==='split'
 *         && !['playing','death','recipes'].includes(type);
 *     head.innerHTML          = inHeader ? notice : '';
 *     notice-mount.innerHTML  = inHeader ? '' : notice;
 *
 * so: a menu that OWNS a header composes the notice INTO that header (the
 * `.head-notice` span: no card background, ink colour, small text, flexed to
 * the right of the title) whenever the viewport is the compact/split one.
 * Every other case -- full-screen layout, and the headerless death screen --
 * keeps the floating dark card in the reserved `.notice-mount` band
 * (position:absolute; top:8; left/right:24) at the top of the affected
 * viewport.
 *
 * NEITHER placement masks a fixed-height strip over a live menu:
 *   - the header reserves the measured card width beside a fixed title, so a
 *     notice appearing or expiring never shifts a header control; and
 *   - the floating band is space no menu may put an actionable widget in.
 * test_menu_header_notice_composition proves the second half for every
 * menu x layout x controller family by declaring each menu's real actionable
 * rectangles through this API and asserting the resolved notice is disjoint
 * from all of them.
 *
 * Pure geometry: no GL, no gfx state, no allocation, no statics. Every
 * composition lives in the caller's frame (Wii rule: no per-frame heap). */

enum menu_screen_id {
	MENU_SCREEN_PAUSE,
	MENU_SCREEN_FURNACE,
	MENU_SCREEN_DISPENSER,
	MENU_SCREEN_SIGN,
	MENU_SCREEN_CHAT, /* the multiplayer chat composer (screen_chat.c) */
	MENU_SCREEN_DEATH,
	MENU_SCREEN_COUNT
};

enum menu_screen_layout {
	MENU_SCREEN_FULL,   /* single-player / full 640x480 viewport */
	MENU_SCREEN_COMPACT /* split viewport (640x240), the compact composition */
};

struct menu_rect {
	int x, y, w, h;
};

/* The reserved floating-notice band, in PHYSICAL pixels, relative to the
 * affected viewport's own origin. Mirrors `.notice-mount {top:8; left:24;
 * right:24}` plus one `.notice` row at the approved 14 px / min-height 28 px,
 * expressed at this engine's GUI scale. */
#define MENU_NOTICE_BAND_TOP 8
#define MENU_NOTICE_BAND_INSET 24
#define MENU_NOTICE_BAND_H 32 /* card A, 10-31 (was 28) */

/* Legacy paged-container controls end before this right margin. The approved
 * title-only menu headers use measured notice_reserve_w instead. */
#define MENU_HEADER_NOTICE_W 120

/* Enough for the largest declaration in the tree: the compact container's 36
 * carried slots + 9 machine slots + 3 header controls + 5 footer prompts = 53.
 * Kept small on purpose -- a composition is a caller stack frame on a console
 * with a 128 KB main stack, not a heap object. */
#define MENU_ACTION_MAX 64

struct menu_composition {
	enum menu_screen_id menu;
	enum menu_screen_layout layout;
	struct menu_rect view;   /* the affected viewport */
	struct menu_rect window; /* the menu panel (== view for death) */
	struct menu_rect header; /* reserved header band; zero when headerless */
	struct menu_rect notice; /* where a controller notice card composes */
	int notice_reserve_w, notice_gap;
	bool has_header;
	bool notice_in_header;
	struct menu_rect actions[MENU_ACTION_MAX];
	size_t action_count;
	/* Declarations dropped because MENU_ACTION_MAX was reached. Counted, never
	 * silently ignored -- a nonzero value fails the golden. */
	size_t action_overflow;
};

/* COMPACT below 480 physical rows, exactly the rule the container views
 * already used; container_layout_for_height now delegates here so there is one
 * authority for "is this the split composition". */
enum menu_screen_layout menu_screen_layout_for(int width, int height);

bool menu_rect_empty(const struct menu_rect* r);
bool menu_rect_overlap(const struct menu_rect* a, const struct menu_rect* b);
bool menu_rect_contains(const struct menu_rect* outer,
						const struct menu_rect* inner);

/* menus.md: "Newly detected devices do not silently take over an occupied
 * player slot", and IMPLEMENTATION.md's open decision: "do not set
 * local_gameplay_ready merely to make a preview appear implemented." The
 * split/compact composition is admitted only once P2 is a real local player
 * WITH explicit body/collision admission; otherwise the owner keeps the
 * single-player composition. */
bool menu_split_composition_admitted(bool second_present, bool second_ready);

/* ---- declaration ------------------------------------------------------- */

void menu_composition_begin(struct menu_composition* c, enum menu_screen_id menu,
							int view_x, int view_y, int view_w, int view_h);
void menu_composition_window(struct menu_composition* c, int x, int y, int w,
							 int h);
/* Declare the header band, then reserve the measured card width with
 * menu_composition_header_notice before sealing the composition. */
void menu_composition_header(struct menu_composition* c, int x, int y, int w,
							 int h);
void menu_composition_header_notice(struct menu_composition*,int desired_width,int title_width);
struct menu_rect
menu_composition_header_content(const struct menu_composition* c);
/* The reserved floating band for a viewport, independent of any declaration --
 * so a menu can keep its widgets out of it before it declares them. */
struct menu_rect menu_notice_band(int view_x, int view_y, int view_w);

void menu_composition_action(struct menu_composition* c, int x, int y, int w,
							 int h);
/* Resolve `notice` / `notice_in_header`. Call once, after every action. */
void menu_composition_seal(struct menu_composition* c);

/* Index of the first declared action the resolved notice would cover, or
 * c->action_count when the composition is clean. */
size_t menu_composition_notice_conflict(const struct menu_composition* c);

/* ---- per-menu geometry (one authority, shared by render and the golden) -- */

/* Pause: `n` is the number of visible actions (2 guest/compact, 3 with
 * Options...). Rows are the approved 200-unit buttons with an 8 px gap. */
#define MENU_PAUSE_BUTTON_UNITS 200
#define MENU_PAUSE_ROW_H (20 * GFX_GUI_SCALE)
#define MENU_PAUSE_ROW_GAP 8
struct menu_rect menu_pause_row(const struct menu_composition* c, int index);
struct menu_rect menu_pause_status(const struct menu_composition* c, int rows);

/* Compact container header: fixed boxes, so the tab row, the Prev/Next
 * prompts and the reserved notice slot cannot collide whatever the tab labels
 * measure under either pack. (wx, wy) is the 640x240 compact content box's
 * top-left in physical pixels. */
#define MENU_CONTAINER_HEADER_Y 8
#define MENU_CONTAINER_HEADER_H 32
struct menu_rect menu_container_header_box(int wx, int wy);
struct menu_rect menu_container_tab_box(int wx, int wy);
struct menu_rect menu_container_prev_box(int wx, int wy);
struct menu_rect menu_container_next_box(int wx, int wy);

/* Sign editor: the reserved character-count area (approved 52 px) sits at the
 * right end of the header CONTENT box, so the line controls never shift as a
 * line approaches the 15-character limit. */
#define MENU_SIGN_COUNT_RESERVE_W 52
struct menu_rect menu_sign_count_box(const struct menu_composition* c);

/* Death: the approved layout puts the actions low in the affected viewport;
 * split stacks them shorter so both fit inside 240 rows. */
#define MENU_DEATH_BUTTON_UNITS 200
struct menu_rect menu_death_row(const struct menu_composition* c, int index);

#endif
