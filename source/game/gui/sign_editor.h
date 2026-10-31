/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
#ifndef GUI_SIGN_EDITOR_H
#define GUI_SIGN_EDITOR_H
#include "container_menu.h"
#include "widget_osk.h"
#include "sign_osk_model.h"
#include "../../platform/menu_sound.h"
struct client_session;
struct input_player_frame;
enum sign_exit_kind {SIGN_EXIT_NONE,SIGN_EXIT_DONE,SIGN_EXIT_CANCEL,SIGN_EXIT_DISCARD};
struct sign_editor {
 const struct window_container* window_identity;
 uint64_t local_generation;
 struct gutil_menu_focus sign_focus;
 bool pointer_available, discard_prompt, closed;
 float pointer_x,pointer_y,pointer_angle;
 size_t selected_slot,osk_line;
 uint8_t sign_container;
 struct osk_state osk;
 char shadow_line[SIGN_OSK_COLS+1];
 uint8_t opened_cells[SIGN_OSK_SIZE];
 uint32_t observed_rejection_serial,exit_rejection;
 enum sign_exit_kind pending_exit;
 enum menu_sound_event exit_cue;
 int discard_selected;
 int discard_origin; /* enum gutil_menu_origin */
 int action_target; /* 0..3 controls, 4 on-screen keyboard */
 int action_origin; /* enum gutil_menu_origin */
};
bool sign_editor_open(struct client_session*,uint8_t window);
/* Frame coordinates are remapped into this owner's viewport; no global input. */
bool sign_editor_update(struct client_session*,unsigned player,const struct input_player_frame*,int x,int y,int width,int height,float dt,container_send_fn,void*);
void sign_editor_render(struct client_session*,unsigned player,const struct input_player_frame*,int width,int height);
#endif
