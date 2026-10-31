/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
#ifndef LOCAL_PAUSE_H
#define LOCAL_PAUSE_H
#include <stdbool.h>
#include "../../graphics/gui_util.h"
struct menu_composition;
struct client_session;
struct input_player_frame;
enum local_pause_result { LOCAL_PAUSE_NONE, LOCAL_PAUSE_BACK, LOCAL_PAUSE_HOST_EXIT };
enum local_pause_page {
 LOCAL_PAUSE_MENU,
 LOCAL_PAUSE_OPTIONS,
 LOCAL_PAUSE_HOST_CONFIRM,
 LOCAL_PAUSE_ACHIEVEMENTS
};
struct local_pause {
 enum local_pause_page page;
 bool open,host,exit_pending,exit_failed,split_off_presented;
 int selected,pointer_target;
 struct gutil_menu_focus focus;
 enum gutil_menu_origin selection_origin;
 enum gutil_menu_origin modal_origin;
 bool pointer_available;
 float pointer_x,pointer_y,pointer_angle;
};
void local_pause_reset(struct local_pause*);
bool local_pause_open(struct local_pause*,bool host);
bool local_pause_active(const struct local_pause*);
void local_pause_compose(struct menu_composition*,int x,int y,int width,int height,int notice_width,int rows);
enum local_pause_result local_pause_update(struct local_pause*,struct client_session*,const struct input_player_frame*,int x,int y,int width,int height);
/* HOST_EXIT is intent only. A rejected enqueue retains a visible retry. */
void local_pause_host_exit_settled(struct local_pause*,bool accepted);
void local_pause_render(const struct local_pause*,unsigned player,const struct input_player_frame*,int x,int y,int width,int height);
#endif
