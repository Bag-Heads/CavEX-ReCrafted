/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later. */
#ifndef GAME_GUI_SCREEN_ACHIEVEMENTS_H
#define GAME_GUI_SCREEN_ACHIEVEMENTS_H

#include "../../graphics/gui_util.h"
#include "menu_header.h"
#include "screen.h"

/* Presentation state only. Progress is read afresh through the owner query
 * API; selecting, Retry, Requirement and Back never mutate the module. */
struct achievements_board {
	bool open, visited, detail, pointer_available;
	uint8_t owner;
	int selected, depth, stack[16];
	bool stack_detail[16];
	struct gutil_menu_focus focus;
	enum gutil_menu_origin origin;
	int pointer_target;
	float pointer_x, pointer_y, pointer_angle;
	char profile[17];
};
enum achievements_availability {
	ACHIEVEMENTS_READY,
	ACHIEVEMENTS_LOADING,
	ACHIEVEMENTS_UNAVAILABLE,
	ACHIEVEMENTS_ERROR
};
enum achievement_visual_state {
	ACHIEVEMENT_LOCKED,
	ACHIEVEMENT_AVAILABLE,
	ACHIEVEMENT_EARNED,
	ACHIEVEMENT_UNKNOWN
};
struct achievements_row {
	uint16_t icon_item;
	int8_t parent;
	bool special, unlocked, can_unlock;
};
struct achievements_data {
	enum achievements_availability state;
	int earned;
	struct achievements_row rows[16];
	char message[112];
};
struct achievements_layout {
	bool split;
	struct menu_rect view, panel, header, chips[2], tabs[4];
	struct menu_rect list_header, rows[6], detail, tiles[7], arrows[2];
	struct menu_rect title, howto,
		requires,
	legend, requirement, back;
};
struct achievements_glyphs {
	int browse, requirement, back, previous, next;
};
enum achievements_action {
	ACHIEVEMENTS_NONE,
	ACHIEVEMENTS_BACK,
	ACHIEVEMENTS_RESUME
};

extern struct screen screen_achievements;
struct achievements_board* screen_achievements_owner(unsigned owner);
void screen_achievements_open(struct achievements_board*, unsigned owner);
void screen_achievements_read(unsigned owner, struct achievements_data*);
enum achievement_visual_state
screen_achievements_state(const struct achievements_data*, int id);
void screen_achievements_layout(struct achievements_layout*, int x, int y,
								int width, int height);
int screen_achievements_tab_of(int id);
int screen_achievements_tab_count(int tab);
int screen_achievements_tab_id(int tab, int row);
const char* screen_achievements_tab_name(int tab);
const char* screen_achievements_name(int id);
int screen_achievements_ribbon_first(int selected);
struct achievements_glyphs
screen_achievements_glyphs(const struct input_player_frame*);
enum achievements_action
screen_achievements_update_owned(struct achievements_board*,
								 const struct input_player_frame*, int x, int y,
								 int width, int height);
/* Uses the caller's GUI projection and owner scissor, as main's per-view
 * menu painter does. Coordinates are local to that projection. */
void screen_achievements_render_owned(const struct achievements_board*,
									  const struct input_player_frame*, int x,
									  int y, int width, int height);
void screen_achievements_return_pause(void);
/* The parent owns pause/resume authority and its remembered row. */
void screen_pause_restore_achievements(void);
bool screen_pause_achievements_focused(void);
bool screen_pause_resume_game(void);
#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void screen_achievements_test_icon(uint16_t item, bool locked, int x, int y,
								   int size);
/* The hint row the last render drew (10-31). */
struct gutil_hint_row;
const struct gutil_hint_row* screen_achievements_test_row(void);
#endif

#endif
