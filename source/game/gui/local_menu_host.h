/* Owner-local compact menus. No global screen stack or gameplay body. */
#ifndef LOCAL_MENU_HOST_H
#define LOCAL_MENU_HOST_H
#include <stdbool.h>
#include <stdint.h>
#include "../../platform/controller_events.h"
#include "container_menu.h"
#include "screen_creative_picker.h"
struct client_session;
struct window_container;
struct input_player_frame;
struct container_glyphs;
enum local_menu_mode { LOCAL_MENU_NONE,LOCAL_MENU_CONTAINER,LOCAL_MENU_RECIPES,LOCAL_MENU_MANUAL,LOCAL_MENU_SIGN,LOCAL_MENU_CREATIVE };
struct local_menu_host {
 enum local_menu_mode mode;
 struct creative_picker picker;
 uint8_t player,window;
 const struct window_container* window_identity;
 struct controller_recovery recovery;
 struct gutil_menu_focus recovery_focus;
 int recovery_origin; /* enum gutil_menu_origin */
 bool split_owned,recovery_pointer;
 bool inventory_open; /* Logical open survives compact/full presentation resets. */
 float recovery_x,recovery_y,recovery_angle;
};
/* Presentation-only reset; the logical inventory open is retained. */
void local_menu_host_reset(struct client_session*);
bool local_menu_host_active(const struct client_session*);
/* The survival inventory GUI was opened successfully for this owner. */
void local_menu_host_inventory_opened(struct client_session*, unsigned player);
/* Retire a real inventory close, independently for this owner. */
void local_menu_host_inventory_closed(struct client_session*);
/* Explicitly open an installed owner mirror; compact viewport only. */
bool local_menu_host_open(struct client_session*,unsigned player,uint8_t window,int width,int height);
bool local_menu_host_open_creative(struct client_session*,unsigned player,int width,int height);
/* Input is already filtered and mapped into this owner's viewport. */
/* True only when a real WINDOW_CLOSE was accepted by the sender. */
bool local_menu_host_update(struct client_session*,const struct container_input*,const struct container_glyphs*,float dt,container_send_fn,void*);
/* Independent of the retained menu or Pause. Resume consumes its own frame. */
bool local_menu_host_recovery_frame(struct client_session*,unsigned player,
 const struct input_player_frame*,int x,int y,int width,int height);
/* critique-1028: the disconnected-controller detail line, worded for the
 * family that was lost: a GameCube pad is wired (plug it back in); every
 * WPAD family (Remote+Nunchuk, Classic) reconnects with 1+2 on its Remote. */
const char* local_menu_host_reconnect_hint(enum input_category category);
void local_menu_host_recovery_render(struct client_session*,
 const struct input_player_frame*,int width,int height);
/* True means this owner menu consumed the frame, including open and close. */
bool local_menu_host_frame(struct client_session*,unsigned player,
 const struct input_player_frame*,int x,int y,int width,int height,float dt,
 container_send_fn,void*);
void local_menu_host_render(struct client_session*,const struct container_glyphs*);
#endif
