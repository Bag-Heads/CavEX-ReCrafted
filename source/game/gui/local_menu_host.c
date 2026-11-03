/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
#include "local_menu_host.h"
#include "../../graphics/gui_util.h"
#include "../client_session.h"
#include "../game_state.h"
#include "../achievements/game_event.h"
#include "container_compact.h"
#include "recipe_browser.h"
#include "../../platform/menu_sound.h"
#include "../../platform/time.h"
#include "../../platform/input_owner.h"
#include "../../platform/input_inventory.h"
#include "../../platform/input_glyph.h"
#include "../../platform/gfx.h"
#include "../../graphics/gfx_settings.h"

static bool recipe_window(const struct window_container* wc) {
 return wc&&((wc->type==WINDOW_TYPE_INVENTORY&&wc->slot_count==INVENTORY_SIZE)
  ||(wc->type==WINDOW_TYPE_WORKBENCH&&wc->slot_count==CRAFTING_SIZE));
}
static unsigned grid(const struct window_container* wc) { return wc->type==WINDOW_TYPE_INVENTORY?2:3; }
static void open_recipes(struct client_session* s,struct window_container* wc) {
 recipe_browser_open(&s->recipes,grid(wc));s->menu_host.mode=LOCAL_MENU_RECIPES;
}
void local_menu_host_reset(struct client_session* s) {
 if(!s)return;
 if(local_menu_host_active(s))input_menu_page_cancel(s->menu_host.player);
 struct local_menu_host old=s->menu_host;
 s->menu_host=(struct local_menu_host){.player=old.player,.recovery=old.recovery,
  .inventory_open=old.inventory_open,
  .recovery_focus=old.recovery_focus,.recovery_pointer=old.recovery_pointer,
  .recovery_x=old.recovery_x,.recovery_y=old.recovery_y,.recovery_angle=old.recovery_angle};
 s->container.active=s->recipes.active=s->manual_grid.active=false;
}
bool local_menu_host_active(const struct client_session* s) { return s&&s->menu_host.mode!=LOCAL_MENU_NONE; }
void local_menu_host_inventory_opened(struct client_session* s, unsigned player) {
	/* Beta RetroMCP GuiInventory.java:13 awards at construction, once per
	 * logical open, not on compact/full presentation changes or containers.
	 * Beta has no creative picker: do not award there
	 * (nor its creative inventory route). ACH-1 MP choice (b): local only. */
	if(!s || player >= CLIENT_LOCAL_PLAYERS || gstate.remote_session
	   || gstate.config_creative || !s->achievement_epoch)
		return;
	if(s->menu_host.inventory_open) return;
	s->menu_host.inventory_open=true;
	game_event_emit(&(struct game_event){.kind = GE_INVENTORY_OPENED,
		.authority = GE_AUTH_CLIENT,
		.who = {.slot = player, .generation = s->achievement_epoch}});
}
void local_menu_host_inventory_closed(struct client_session* s) {
 if(s)s->menu_host.inventory_open=false;
}
static void retire_hidden_inventory(struct client_session* s) {
 /* RETURN_INGAME resets compact menus even when no global screen changes.
  * Retire at the next GUI frame/open. Full-screen reset and main's reverse
  * conversion already select screen_inventory before they reopen the host. */
 if(!local_menu_host_active(s)&&s->current_screen!=&screen_inventory)
  local_menu_host_inventory_closed(s);
}
bool local_menu_host_open(struct client_session* s,unsigned player,uint8_t window,int width,int height) {
 if(!s||player>=CLIENT_LOCAL_PLAYERS||width<640||height<240||height>=480
  ||!s->windows[window]||!s->windows[WINDOWC_INVENTORY])return false;
 retire_hidden_inventory(s);
 struct window_container* wc=s->windows[window];
 if(wc->type==WINDOW_TYPE_SIGN&&wc->slot_count==SIGN_OSK_SIZE) {
  if(s->sign.window_identity!=wc||s->sign.local_generation!=s->local_generation||s->sign.closed)
   if(!sign_editor_open(s,window))return false;
  local_menu_host_inventory_closed(s);
  local_menu_host_reset(s);
  s->container.width=width;s->container.height=height;
  s->menu_host.mode=LOCAL_MENU_SIGN;s->menu_host.player=(uint8_t)player;
  s->menu_host.window=window;s->menu_host.window_identity=wc;
  return true;
 }
 struct inventory* inv=windowc_get_latest(s->windows[WINDOWC_INVENTORY]);
 struct container_compact compact;
 if(!container_compact_open(&compact,wc->type,wc->slot_count,inventory_get_hotbar(inv),width,height))return false;
 if(window!=WINDOWC_INVENTORY||wc->type!=WINDOW_TYPE_INVENTORY)
  local_menu_host_inventory_closed(s);
 local_menu_host_reset(s);s->container=compact;s->container.menu.sort_owner=s;
 input_menu_page_cancel(player);
 s->menu_host.mode=LOCAL_MENU_CONTAINER;s->menu_host.player=(uint8_t)player;
 s->menu_host.window=window;s->menu_host.window_identity=wc;
 if(wc->type==WINDOW_TYPE_WORKBENCH)open_recipes(s,wc);
 if(window==WINDOWC_INVENTORY && wc->type==WINDOW_TYPE_INVENTORY)
  local_menu_host_inventory_opened(s,player);
 return true;
}
bool local_menu_host_open_creative(struct client_session* s,unsigned player,int width,int height) {
 if(!s||player>=CLIENT_LOCAL_PLAYERS||width<640||height<240||height>=480
  ||!gstate.config_creative)return false;
 local_menu_host_reset(s);
 creative_picker_open(&s->menu_host.picker);
 s->container.width=width;s->container.height=height;
 s->menu_host.mode=LOCAL_MENU_CREATIVE;s->menu_host.player=(uint8_t)player;
 s->menu_host.split_owned=true;
 return true;
}
static void poll_craft(struct client_session* s) {
 struct recipe_browser* b=&s->recipes;
 if(!b->pending||(s->craft_pending&&s->craft_reply_serial==b->pending_serial))return;
 recipe_browser_craft_reply(b,s->craft_pending?RECIPE_CRAFT_TIMED_OUT:s->craft_reason);
}
/* ui-build-1026/T12: the personal inventory has two pages, Inventory and
 * Crafting; a tap of PAGE_SWAP is the ONLY way between them. The old entries
 * (section-cycle wrap, header arrows and the "Recipes" button) are gone, so
 * -/+ stay section jumps here and category tabs on the Crafting page. A swap
 * is presentation only: no RPC, the window, its cursor stack and the real 2x2
 * craftMatrix slots are untouched (Beta ContainerPlayer.java:13 -- one
 * container for the whole GUI; emptied only at close, :48-57). The workbench
 * keeps its single 3x3 guide unchanged. */
/* Sol review 10-31 items 1+2: the bottom row's "Crafting" / "Inventory" cells
 * are page swaps too, resolved here from the same layout the painter uses
 * (this test runs BEFORE the step that used to resolve the click, so the
 * click was eaten). */
static bool page_swap_ready(const struct client_session* s,const struct window_container* wc,
 const struct container_input* in,const struct container_glyphs* glyphs) {
 const struct container_compact* c=&s->container;
 bool swap=in->page_swap
  ||(s->menu_host.mode==LOCAL_MENU_CONTAINER&&container_compact_row_page_swap(c,wc,glyphs,in))
  ||(s->menu_host.mode==LOCAL_MENU_RECIPES&&recipe_browser_row_page_swap(&s->recipes,glyphs,in,c->width,c->height));
 return swap&&!in->suspended&&!in->close&&!c->menu.sort_open
  &&!c->menu.close_requested&&!c->menu.suspended;
}
static bool close_workbench(struct client_session* s,struct window_container* wc,
 const struct container_input* in,float dt,container_send_fn send,void* user) {
 struct container_input close={.close=true,.suspended=in->suspended};
 if(!container_compact_step(&s->container,wc,s->menu_host.window,&close,dt,send,user))return false;
 local_menu_host_reset(s);return true;
}
static bool update_recipes(struct client_session* s,struct window_container* wc,
 const struct container_input* in,const struct container_glyphs* glyphs,float dt,container_send_fn send,void* user) {
 struct recipe_browser* b=&s->recipes;
 if(s->container.menu.close_requested)return close_workbench(s,wc,in,dt,send,user);
 poll_craft(s);recipe_browser_refresh_availability(b,wc,s->craft_pending||s->local_reply_failed);
 if(wc->type==WINDOW_TYPE_INVENTORY&&page_swap_ready(s,wc,in,glyphs)&&!b->pending&&!b->close_after_craft) {
  b->active=b->details=false;s->menu_host.mode=LOCAL_MENU_CONTAINER;
  audio_menu_event(MENU_SOUND_SECTION,time_get());return false;
 }
 enum recipe_browser_action action=recipe_browser_step_owned(b,in,glyphs,s->container.width,s->container.height,dt);
 if(action==RECIPE_BROWSER_CRAFT) {
  uint16_t recipe=recipe_browser_selected_index(b);
  if(recipe!=RECIPE_CRAFT_INDEX_NONE&&client_session_recipe_craft_request(s,s->menu_host.player,s->menu_host.window,recipe,send,user)) {
   b->pending=true;b->has_reason=false;b->pending_serial=s->craft_reply_serial;
  } else recipe_browser_craft_reply(b,RECIPE_CRAFT_BUSY);
 } else if(action==RECIPE_BROWSER_MANUAL&&wc->type==WINDOW_TYPE_INVENTORY) {
  /* T12 fix r1: no Manual Grid popup for the personal grid. The shortcut
   * goes to the page holding the REAL 2x2 slots and focuses its first slot. */
  b->active=b->details=false;s->menu_host.mode=LOCAL_MENU_CONTAINER;
  container_compact_focus_slot(&s->container,INVENTORY_SLOT_CRAFTING);
  audio_menu_event(MENU_SOUND_SECTION,time_get());
 } else if(action==RECIPE_BROWSER_MANUAL) {
  manual_grid_open(&s->manual_grid,grid(wc),s->container.width,s->container.height);
  s->container.menu.return_blocked=s->container.menu.send_failed=false;
  s->manual_grid.owner_index=s->menu_host.player;b->active=false;s->menu_host.mode=LOCAL_MENU_MANUAL;
  audio_menu_event(MENU_SOUND_SECTION,time_get());
 } else if(action==RECIPE_BROWSER_CLOSE) {
  /* Close at the root: both pages close the window the same way. */
  return close_workbench(s,wc,in,dt,send,user);
 }
 return false;
}
bool local_menu_host_update(struct client_session* s,const struct container_input* in,
 const struct container_glyphs* glyphs,float dt,container_send_fn send,void* user) {
 if(!s||!in||!glyphs||!send||!local_menu_host_active(s)||s->menu_host.mode==LOCAL_MENU_SIGN||s->menu_host.mode==LOCAL_MENU_CREATIVE)return false;
 struct window_container* wc=s->windows[s->menu_host.window];
 if(!wc||wc!=s->menu_host.window_identity||wc->type!=s->container.type||wc->slot_count!=s->container.main_base+36){local_menu_host_inventory_closed(s);local_menu_host_reset(s);return false;}
 s->container.menu.sort_owner=s;
 /* Consume recovery before paging, recipe wrapping, or slot actions. Each
  * presentation clears its suspension with neutral input on reconnection. */
 bool was_suspended=s->menu_host.mode==LOCAL_MENU_RECIPES?s->recipes.suspended:
  s->menu_host.mode==LOCAL_MENU_MANUAL?s->manual_grid.menu.suspended:s->container.menu.suspended;
 struct container_input neutral={.suspended=in->suspended||s->local_reply_failed};
 if(neutral.suspended||was_suspended)in=&neutral;
 switch(s->menu_host.mode) {
 case LOCAL_MENU_CONTAINER:
  if(wc->type==WINDOW_TYPE_INVENTORY&&recipe_window(wc)&&page_swap_ready(s,wc,in,glyphs)){
   open_recipes(s,wc);audio_menu_event(MENU_SOUND_SECTION,time_get());return false;
  }
  if(container_compact_step(&s->container,wc,s->menu_host.window,in,dt,send,user)){local_menu_host_inventory_closed(s);local_menu_host_reset(s);return true;}
  break;
 case LOCAL_MENU_RECIPES:return update_recipes(s,wc,in,glyphs,dt,send,user);
 case LOCAL_MENU_MANUAL:
  if(manual_grid_step(&s->manual_grid,wc,s->menu_host.window,in,glyphs,dt,send,user)) {
   s->recipes.active=true;s->recipes.details=false;s->menu_host.mode=LOCAL_MENU_RECIPES;
   audio_menu_event(MENU_SOUND_BACK,time_get());
  }
  break;
 case LOCAL_MENU_CREATIVE:case LOCAL_MENU_SIGN:case LOCAL_MENU_NONE:break;
 }
 return false;
}
/* The local (split pane) card, from the same strings the render shows. */
static bool local_recovery_card(struct local_menu_host* host,
 const struct input_player_frame* frame,struct controller_recovery_card* card) {
 if(!controller_recovery_card(&host->recovery,frame?frame->category:INPUT_CAT_NUNCHUK,true,card))return false;
 if(host->recovery.phase==CONTROLLER_RECOVERY_LOST
   &&host->recovery.reason==CONTROLLER_EVENT_DISCONNECTED)
  snprintf(card->detail,sizeof card->detail,"%s",
   local_menu_host_reconnect_hint(frame?frame->category:INPUT_CAT_NUNCHUK));
 return true;
}
static struct gutil_button_bounds recovery_button(struct local_menu_host* host,
 const struct input_player_frame* frame,int width,int height) {
 struct controller_recovery_card card;struct recovery_card_layout l;
 if(!local_recovery_card(host,frame,&card))return (struct gutil_button_bounds){0,0,0,0};
 recovery_card_layout(0,0,width,height,true,&card,true,frame?frame->glyphs[IB_GUI_CLICK]:-1,&l);
 return l.button;
}
#ifdef PLATFORM_WII
static bool recovery_category_playable(enum input_category category) {
 return category==INPUT_CAT_NUNCHUK||category==INPUT_CAT_CLASSIC_CONTROLLER
  ||category==INPUT_CAT_GAMECUBE;
}
#endif
bool local_menu_host_recovery_frame(struct client_session* s,unsigned player,
 const struct input_player_frame* frame,int x,int y,int width,int height) {
 if(!s||player>=CLIENT_LOCAL_PLAYERS||width<=0||height<=0)return false;
 struct input_player_frame missing={0};
 if(!frame)frame=&missing;
 struct local_menu_host* host=&s->menu_host;host->player=(uint8_t)player;
 struct controller_recovery_input in={.paused=true}; /* local modal never pauses the world */
 struct controller_recovery next;struct input_device device;
 const struct input_device_status* status=input_player_device(player,&device)
  ?input_inventory_device(input_devices(),device):NULL;
 if(status) {
  in.present=status->observed.connected&&status->published.connected
   &&(status->observed.category==INPUT_CAT_NUNCHUK||status->observed.category==INPUT_CAT_CLASSIC_CONTROLLER||status->observed.category==INPUT_CAT_GAMECUBE)
   &&(status->published.category==INPUT_CAT_NUNCHUK||status->published.category==INPUT_CAT_CLASSIC_CONTROLLER||status->published.category==INPUT_CAT_GAMECUBE);
  in.neutral=status->armed;
  in.reason=status->observed.connected?CONTROLLER_EVENT_ATTACHMENT_MISSING:CONTROLLER_EVENT_DISCONNECTED;
 } else {
#ifdef PLATFORM_WII
  in.present=in.neutral=frame->connected&&recovery_category_playable(frame->category);
  in.reason=frame->connected?CONTROLLER_EVENT_ATTACHMENT_MISSING:CONTROLLER_EVENT_DISCONNECTED;
#else
  /* PC keyboard owners have no hardware inventory and carry INPUT_CAT_NONE. */
  in.present=in.neutral=frame->connected;
  in.reason=CONTROLLER_EVENT_DISCONNECTED;
#endif
 }
 /* A replacement channel can already be published as connected. Its
  * claim press disarms input; require Resume even if the old channel's
  * loss and the replacement claim occurred in the same input poll. */
 if(in.present&&!in.neutral&&host->recovery.phase==CONTROLLER_RECOVERY_NONE)
  host->recovery.phase=CONTROLLER_RECOVERY_NEUTRAL;
 bool gated=host->recovery.phase!=CONTROLLER_RECOVERY_NONE;
 host->recovery_pointer=input_owner_pointer(frame,x,y,width,height,
  &host->recovery_x,&host->recovery_y,&host->recovery_angle);
 struct gutil_menu_decision decision={0};
 if(gated&&frame->connected&&host->recovery.phase==CONTROLLER_RECOVERY_READY) {
  bool direction=frame->pressed[IB_GUI_UP]||frame->pressed[IB_GUI_DOWN]
   ||frame->pressed[IB_GUI_LEFT]||frame->pressed[IB_GUI_RIGHT];
  bool pointer_owner=gutil_menu_pointer_focus(&host->recovery_focus,
   host->recovery_pointer,host->recovery_x,host->recovery_y,direction);
  if(direction)host->recovery_origin=GUTIL_MENU_ORIGIN_DIRECTIONAL;
  else if(pointer_owner)host->recovery_origin=GUTIL_MENU_ORIGIN_POINTER;
  struct gutil_button_bounds button=recovery_button(host,frame,width,height);
  int hovered=host->recovery_pointer&&gutil_button_hit(&button,
   host->recovery_x,host->recovery_y)?0:GUTIL_MENU_TARGET_NONE;
  /* The sprite takes part only while it OWNS focus -- exactly what the
   * renderer shows (local_menu_host_recovery_render rings Resume whenever the
   * sprite does not own focus). An unmoved sprite over blank therefore leaves
   * A on Resume under rule (3), as the pre-G4 gate did (Fable G4 review 4). */
  decision=gutil_menu_activate_decide((struct gutil_menu_activate_state){
   .pressed=frame->pressed[IB_GUI_CLICK],.navigated=direction,
   .pointer_valid=host->recovery_pointer&&pointer_owner,.selection_origin=host->recovery_origin,
   .hovered=hovered,.hovered_enabled=hovered==0,
   .selected=0,.selected_enabled=true});
  host->recovery_origin=decision.selection_origin;
 } else {
  host->recovery_origin=GUTIL_MENU_ORIGIN_DEFAULT;
  /* Seeded on every frame before READY, so READY is entered seeded: the
   * first valid IR frame then does NOT make a resting sprite the focus
   * owner -- only purposeful motion does (Fable G4 review 4). */
  host->recovery_focus=(struct gutil_menu_focus){.initialized=true};
 }
 in.resume=gated&&frame->connected&&host->recovery.phase==CONTROLLER_RECOVERY_READY
  &&decision.action==GUTIL_MENU_ACTIVATE;
 enum controller_recovery_action action=controller_recovery_step(&host->recovery,in,&next);
 controller_recovery_commit(&host->recovery,&next,action,true);
 if(action==CONTROLLER_RECOVERY_ACT_DISMISS) {
  audio_menu_event(MENU_SOUND_BACK,time_get());host->recovery_focus=(struct gutil_menu_focus){.initialized=true};
  host->recovery_origin=GUTIL_MENU_ORIGIN_DEFAULT;
 }
 gated|=host->recovery.phase!=CONTROLLER_RECOVERY_NONE;
 if(gated)for(int button=0;button<IB_COUNT;button++)
  if(frame->down[button]||frame->pressed[button]||frame->released[button])
   input_player_block(player,(enum input_button)button);
 return gated;
}
bool local_menu_host_frame(struct client_session* s,unsigned player,
 const struct input_player_frame* frame,int x,int y,int width,int height,float dt,
 container_send_fn send,void* user) {
 if(!s||!send||player>=CLIENT_LOCAL_PLAYERS)return false;
 retire_hidden_inventory(s);
 bool consumed=local_menu_host_active(s);
 if(!frame)return consumed;
 if(consumed&&s->menu_host.mode!=LOCAL_MENU_CREATIVE) {
  uint8_t w=s->menu_host.window;
  bool visible=s->visible_window==w||(w==WINDOWC_INVENTORY&&s->visible_window<0);
  if(!visible||s->windows[w]!=s->menu_host.window_identity){local_menu_host_inventory_closed(s);local_menu_host_reset(s);}
 }
 if(local_menu_host_recovery_frame(s,player,frame,x,y,width,height)){
  input_menu_page_cancel(player);return true;
 }
 if(!local_menu_host_active(s)) {
  int desired=s->visible_window;
  /* brief 07 C: the guest's GAMEPLAY OPEN, same role split as the host's. */
  if(desired<0&&frame->connected&&!s->local_reply_failed&&frame->pressed[screen_ingame_action_role[SCREEN_INGAME_ACT_OPEN_INVENTORY]]) {
   if(gstate.config_creative)return local_menu_host_open_creative(s,player,width,height);
   desired=WINDOWC_INVENTORY;
  }
  if(desired>=0&&desired<=UINT8_MAX&&local_menu_host_open(s,player,(uint8_t)desired,width,height)){s->menu_host.split_owned=true;return true;}
  return consumed;
 }
 if(s->menu_host.mode==LOCAL_MENU_CREATIVE) {
  if(!s->local_reply_failed) {
   enum creative_picker_action action=creative_picker_update(&s->menu_host.picker,
    frame,x,y,width,height,send,user);
   if(action==CREATIVE_PICKER_CLOSE)local_menu_host_reset(s);
   else if(action==CREATIVE_PICKER_INVENTORY&&local_menu_host_open(s,player,WINDOWC_INVENTORY,width,height))
    s->menu_host.split_owned=true;
  }
  return true;
 }
 if(s->menu_host.mode==LOCAL_MENU_SIGN) {
  uint8_t closing=s->menu_host.window;
  if(sign_editor_update(s,player,frame,x,y,width,height,dt,send,user)) {
   local_menu_host_reset(s);if(s->visible_window==closing)s->visible_window=-1;
  }
  return true;
 }
 struct container_input input;
 container_menu_read_player_owned(&input,player,frame,x,y,width,height);
 struct container_glyphs glyphs;container_compact_player_glyphs(&glyphs,frame);
 uint8_t closing=s->menu_host.window;
 if(local_menu_host_update(s,&input,&glyphs,dt,send,user)&&s->visible_window==closing)s->visible_window=-1;
 return true;
}
static void render_notices(struct client_session* s,struct window_container* wc) {
 unsigned player=s->menu_host.player;
 if(s->menu_host.mode==LOCAL_MENU_RECIPES||s->menu_host.mode==LOCAL_MENU_MANUAL) {
  gutil_connection_notices_mini(player,true,(s->container.width-640)/2+24,
   (s->container.height-240)/2,592,16);
 } else {
  struct gutil_button_bounds r;
  container_compact_notice_bounds(&s->container,wc,gutil_connection_notice_width(player,true,true),&r);
  gutil_connection_notices_in_header(player,true,r.x,r.y,r.width,r.height);
 }
}
void local_menu_host_render(struct client_session* s,const struct container_glyphs* glyphs) {
 if(!s||!glyphs||!local_menu_host_active(s))return;
 if(s->menu_host.mode==LOCAL_MENU_CREATIVE) {
  creative_picker_render(&s->menu_host.picker,s->menu_host.player,s->container.width,s->container.height);
  return;
 }
 struct window_container* wc=s->windows[s->menu_host.window];if(!wc||wc!=s->menu_host.window_identity)return;
 if(s->menu_host.mode==LOCAL_MENU_SIGN) {
  struct input_player_frame frame;input_prompt_frame(s->menu_host.player,&frame);
  sign_editor_render(s,s->menu_host.player,&frame,s->container.width,s->container.height);return;
 }
 switch(s->menu_host.mode) {
 case LOCAL_MENU_CONTAINER:container_compact_render(&s->container,wc,0,0,glyphs);break;
 case LOCAL_MENU_RECIPES:
  s->recipes.close_status[0]=0;
  if(s->container.menu.close_requested||s->container.menu.return_blocked||s->container.menu.send_failed)
   container_menu_label(&s->container.menu,wc,0,s->recipes.close_status,sizeof s->recipes.close_status);
  recipe_browser_render(&s->recipes,windowc_get_latest(wc),wc->type==WINDOW_TYPE_INVENTORY?INVENTORY_SLOT_MAIN:CRAFTING_SLOT_MAIN,
   0,0,s->container.width,s->container.height,glyphs);break;
 case LOCAL_MENU_MANUAL:manual_grid_render(&s->manual_grid,wc,0,0,glyphs);break;
 case LOCAL_MENU_CREATIVE:case LOCAL_MENU_SIGN:case LOCAL_MENU_NONE:break;
 }
 render_notices(s,wc);
}

const char* local_menu_host_reconnect_hint(enum input_category category) {
 return category==INPUT_CAT_GAMECUBE?"Plug the controller back in."
  :"Press 1+2 on the Wii Remote.";
}

void local_menu_host_recovery_render(struct client_session* s,
 const struct input_player_frame* frame,int width,int height) {
 if(!s||width<640||height<240)return;
 struct local_menu_host* host=&s->menu_host;struct controller_recovery_card card;
 if(!local_recovery_card(host,frame,&card))return;
 bool ready=host->recovery.phase==CONTROLLER_RECOVERY_READY;
 int glyph=frame?frame->glyphs[IB_GUI_CLICK]:-1;
 struct recovery_card_layout l;
 recovery_card_layout(0,0,width,height,true,&card,ready,glyph,&l);
 gfx_texture(false);
 gutil_texquad_col(0,0,0,0,0,0,width,height,0,0,0,153);
 gfx_texture(true);
 bool focus=ready&&(!host->recovery_focus.pointer_owner||(host->recovery_pointer
  &&gutil_button_hit(&l.button,host->recovery_x,host->recovery_y)));
 recovery_card_draw(&l,&card,glyph,focus);
 if(host->recovery_pointer) {
  gfx_bind_texture(&texture_pointer);
  gutil_texquad_rt_any(host->recovery_x,host->recovery_y,glm_rad(host->recovery_angle),0,0,256,256,48,48);
 }
}
