#ifndef SCREEN_DEATH_H
#define SCREEN_DEATH_H

#include <stdbool.h>
#include <stdint.h>

struct client_session;
struct input_player_frame;

enum death_commit {
	DEATH_COMMIT_NONE,
	DEATH_COMMIT_RESPAWN,
	DEATH_COMMIT_EXIT
};

struct death_ui {
	bool active;
	int selected;
	bool pointer_available;
	float pointer_x, pointer_y, pointer_angle;
	float grace_s, hold_s;
	bool click_armed;
	int hold_target;
	enum death_commit commit_pending;
	uint64_t request_generation, completed_floor;
	uint32_t failure_serial;
	bool host_local_respawn;
	bool exit_pending, exit_failed;
};

void screen_death_host_respawn_poll(void);
void screen_death_owner_reset(struct client_session* owner);
void screen_death_owner_enter(struct client_session* owner);
bool screen_death_owner_update(struct client_session* owner, unsigned player,
	const struct input_player_frame* frame, float dt,
	int viewport_x, int viewport_y, int width, int height);
void screen_death_owner_render(struct client_session* owner, unsigned player,
	int width, int height);
bool screen_death_owner_exit_settled(struct client_session* owner,bool success);

/* critique-1028 death redesign: the pure layout of the death screen's two
 * hold buttons and its footer (screen_death.c screen_death_layout). Widths
 * are in pixels: the two button labels, the D-pad and A glyph cells (32 px
 * cell family) and the two footer labels in the 16 px font. fill[k] in [0,1]
 * is the hold progress of button k. */
#include "menu_header.h"
#define DEATH_FOOTER_MOVE "Move"
#define DEATH_FOOTER_HOLD "Hold to choose"
/* The A1 hint row (controller UI redesign 10-31): 24 px glyph cells, label
 * 5 px after (GUTIL_HINT_LABEL_GAP), items 14 px apart (GUTIL_HINT_GAP), the
 * glyph row 2 px into the 28 px strip that ends 24 px (full) / 16 px (split)
 * above the view bottom: y 430 at 480, 198 in a 240 pane. */
#define DEATH_FOOTER_LABEL_GAP 5
#define DEATH_FOOTER_ITEM_GAP 14
#define DEATH_FOOTER_Y_FULL 430
#define DEATH_FOOTER_Y_COMPACT 198
struct death_screen_layout {
	struct menu_rect button[2];
	int label_x[2], label_y[2];
	struct menu_rect fill[2], line[2];
	int footer_x, footer_y, footer_w;
	int move_cell_x, move_label_x, hold_cell_x, hold_label_x;
};
void screen_death_layout(int width, int height, const int label_w[2],
	const float fill[2], int dpad_w, int a_w, int move_w, int hold_w,
	struct death_screen_layout* out);

#endif

