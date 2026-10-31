/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifndef GAME_GUI_ACHIEVEMENT_NOTICE_H
#define GAME_GUI_ACHIEVEMENT_NOTICE_H

#include "../../graphics/gui_util.h"
#include "../../platform/input_owner.h"
#include "../../platform/time.h"
#include "menu_header.h"

#define ACHIEVEMENT_NOTICE_WINDOW_MS 3000
#define ACHIEVEMENT_NOTICE_SLIDE_MS 375

struct achievement_notice_frame {
	struct menu_rect card, slot, caption, name;
	int ach_id, glyph, slide_y;
	unsigned elapsed_ms;
	bool hint, wrapped;
};

/* Client main thread. update runs once EVERY frame, including hidden menus.
 * The module retains each owner's FIFO; the presenter acknowledges only the
 * completed front. Identity/generation changes discard presentation state. */
void achievement_notice_init(void);
void achievement_notice_shutdown(void);
void achievement_notice_update(ptime_t now);

/* Absolute coordinates within the caller's currently scissored viewport.
 * blocked = menus, full map, death/recovery or any other owner modal.
 * Occupied HUD rectangles reserve eight extra pixels around the whole
 * animation path; no safe top-right position means defer without restarting.
 * out, when supplied, reports what the REAL draw presented (empty if hidden).
 * Live inventory glyph comes exclusively from this owner's prompt frame. */
bool achievement_notice_render(unsigned owner,
							   const struct input_player_frame* input, int x,
							   int y, int w, int h, bool blocked,
							   const struct menu_rect* occupied,
							   size_t occupied_count,
							   struct achievement_notice_frame* out);
void achievement_notice_layout(struct achievement_notice_frame* f, int x, int y,
							   int w, int h, int ach_id, bool hint,
							   const struct input_player_frame* input);

#endif
