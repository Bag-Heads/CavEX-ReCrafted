/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
#include <stdio.h>
#include "local_pause.h"
#include "menu_header.h"
#include "../../graphics/gui_util.h"
#include "../../graphics/gfx_settings.h"
#include "../../platform/gfx.h"
#include "../../platform/input_inventory.h"
#include "../../platform/input_owner.h"
#include "../../platform/menu_sound.h"
#include "../../platform/texture.h"
#include "../../platform/time.h"
#include "../client_session.h"
#include "../game_state.h"
#include "screen.h"
#include "screen_achievements.h"
#include "screen_stats_save.h"
#include "../achievements/achievements.h"
static int pause_rows(const struct local_pause* p) {
 return p&&p->page==LOCAL_PAUSE_MENU?4:2;
}
static const char* pause_label(const struct local_pause* p,int row) {
 if(p->page==LOCAL_PAUSE_HOST_CONFIRM)return row?"Confirm save and quit":"Cancel";
 if(p->page==LOCAL_PAUSE_OPTIONS)return row?"Back":screen_options_split_label();
 if(!row)return "Back to game";
 if(row==1)return "Achievements";
 if(row==2)return "Options...";
 if(p->exit_failed)return p->host?"Retry save and quit":"Retry leave";
 return p->host?"Save and quit":"Leave game";
}
void local_pause_reset(struct local_pause* p) { if(p)*p=(struct local_pause){0}; }
bool local_pause_open(struct local_pause* p,bool host) {
 if(!p||p->open)return false;
 *p=(struct local_pause){.open=true,.host=host,.pointer_target=-1,.focus={.initialized=true}};
 audio_menu_event(MENU_SOUND_SECTION,time_get());return true;
}
bool local_pause_active(const struct local_pause* p) { return p&&p->open; }
void local_pause_compose(struct menu_composition* c,int x,int y,int w,int h,int notice,int rows) {
 menu_composition_begin(c,MENU_SCREEN_PAUSE,x,y,w,h);
 if(c->layout==MENU_SCREEN_FULL) {
  menu_composition_window(c,x+(w-360)/2,y+44,360,rows>2?284+(rows-3)*48:268);
  menu_composition_header(c,x+(w-360)/2+12,y+56,336,40);
 } else {
  int pw=w>=854?720:576;
  menu_composition_window(c,x+(w-pw)/2,y+12,pw,216);
  menu_composition_header(c,x+(w-pw)/2+16,y+12,pw-32,24);
 }
 for(int k=0;k<rows;k++) {
  struct menu_rect r;
  if(c->layout==MENU_SCREEN_FULL)r=menu_pause_row(c,k);
  else if(rows==4) {
   int pw=w>=854?720:576,bw=(pw-44)/2;
   r=(struct menu_rect){x+(w-pw)/2+16+(k%2)*(bw+12),y+68+(k/2)*52,bw,44};
  } else r=(struct menu_rect){x+(w-288)/2,y+68+k*52,288,44};
  menu_composition_action(c,r.x,r.y,r.w,r.h);
 }
 menu_composition_header_notice(c,notice,gutil_font_width("Game menu",16));menu_composition_seal(c);
}
static void select_row(struct local_pause* p,int n,int rows) {
 if(n<0||n>=rows||n==p->selected)return;
 p->selected=n;audio_menu_event(MENU_SOUND_FOCUS,time_get());
}
static enum local_pause_result go_back(struct local_pause* p) {
 if(p->exit_pending)return LOCAL_PAUSE_NONE;
 if(p->page==LOCAL_PAUSE_OPTIONS) {
  if(screen_options_split_off_phase()!=SCREEN_SPLIT_OFF_IDLE)return LOCAL_PAUSE_NONE;
  p->page=LOCAL_PAUSE_MENU;p->selected=2;p->exit_failed=false;
  p->focus=(struct gutil_menu_focus){.initialized=true};
  p->selection_origin=GUTIL_MENU_ORIGIN_DEFAULT;
  audio_menu_event(MENU_SOUND_BACK,time_get());return LOCAL_PAUSE_NONE;
 }
 if(p->page==LOCAL_PAUSE_HOST_CONFIRM) {
  p->page=LOCAL_PAUSE_MENU;p->selected=3;p->exit_failed=false;
  audio_menu_event(MENU_SOUND_BACK,time_get());return LOCAL_PAUSE_NONE;
 }
 p->open=false;audio_menu_event(MENU_SOUND_BACK,time_get());return LOCAL_PAUSE_BACK;
}
static void guest_stats_saved(void* user, enum stats_exit_result result) {
 struct client_session* s = user;
 if(result == STATS_EXIT_BACK) return;
 if(result == STATS_EXIT_WITHOUT_SAVE) achievements_request_rescue(1);
 if(client_session_request_local_leave(s)) { s->pause.exit_pending = true; s->pause.exit_failed = false; }
 else s->pause.exit_failed = true;
}
static enum local_pause_result activate(struct local_pause* p,struct client_session* s) {
 if(p->exit_pending)return LOCAL_PAUSE_NONE;
 if(p->page==LOCAL_PAUSE_OPTIONS) {
  if(p->selected==1)return go_back(p);
  struct split_live_status live=screen_options_split_live_status();
  if(live.pending)return LOCAL_PAUSE_NONE;
  if(screen_options_split_set(!live.live)) {
   p->split_off_presented=screen_options_split_off_phase()!=SCREEN_SPLIT_OFF_IDLE;
   if(p->split_off_presented) {
    p->selected=0;p->focus=(struct gutil_menu_focus){0};
    p->modal_origin=GUTIL_MENU_ORIGIN_DEFAULT;
   }
   audio_menu_event(MENU_SOUND_SECTION,time_get());
  }
  return LOCAL_PAUSE_NONE;
 }
 if(p->page==LOCAL_PAUSE_HOST_CONFIRM) {
  if(!p->selected)return go_back(p);
  p->exit_pending=true;p->exit_failed=false;return LOCAL_PAUSE_HOST_EXIT;
 }
 if(!p->selected)return go_back(p);
 if(p->selected==1) {
  /* main opens host=true only for player 0; guest=false only for player 1.
   * The child shares that explicit owner and never changes the global screen. */
  unsigned owner=p->host?0:1;
  screen_achievements_open(screen_achievements_owner(owner),owner);
  p->page=LOCAL_PAUSE_ACHIEVEMENTS;return LOCAL_PAUSE_NONE;
 }
 if(!p->host&&p->selected==2)return LOCAL_PAUSE_NONE;
 if(p->host) {
  if(p->selected==2) {
   p->page=LOCAL_PAUSE_OPTIONS;p->selected=0;p->exit_failed=false;
   p->focus=(struct gutil_menu_focus){.initialized=true};
   p->selection_origin=GUTIL_MENU_ORIGIN_DEFAULT;
   audio_menu_event(MENU_SOUND_SECTION,time_get());return LOCAL_PAUSE_NONE;
  }
  p->page=LOCAL_PAUSE_HOST_CONFIRM;p->selected=0;p->exit_failed=false;
  audio_menu_event(MENU_SOUND_SECTION,time_get());return LOCAL_PAUSE_NONE;
 }
 if(s) screen_stats_save_open(1, 2u, guest_stats_saved, s);
 else p->exit_failed=true;
 return LOCAL_PAUSE_NONE;
}
static struct menu_rect split_modal_panel(int w,int h) {
 int pw=w-168;if(pw>472)pw=472;if(pw<360)pw=360;
 int ph=h>=480?220:180;
 return (struct menu_rect){(w-pw)/2,(h-ph)/2,pw,ph};
}
static struct menu_rect split_modal_button(int w,int h,int index) {
 struct menu_rect p=split_modal_panel(w,h);int gap=28;
 int bw=(p.w-gap*3)/2,bh=h>=480?44:32;
 return (struct menu_rect){p.x+gap+index*(bw+gap),p.y+p.h-bh-24,bw,bh};
}
static bool frame_active(const struct input_player_frame* f) {
 if(!f)return false;
 for(int b=0;b<IB_COUNT;b++)if(f->down[b]||f->pressed[b]||f->released[b])return true;
 return false;
}
static bool split_modal_update(struct local_pause* p,
 const struct input_player_frame* f,int w,int h) {
 enum screen_options_split_off_phase phase=screen_options_split_off_phase();
 if(phase==SCREEN_SPLIT_OFF_IDLE) {
  if(p->split_off_presented) {
   p->split_off_presented=false;p->selected=0;
   p->focus=(struct gutil_menu_focus){.initialized=true};
   p->modal_origin=GUTIL_MENU_ORIGIN_DEFAULT;
  }
  return false;
 }
 p->split_off_presented=true;
 bool actionable=phase==SCREEN_SPLIT_OFF_CONFIRM
  ||phase==SCREEN_SPLIT_OFF_LEAVE_FAILED
  ||phase==SCREEN_SPLIT_OFF_REQUEST_FAILED;
 if(!actionable)return true;
 int first=phase==SCREEN_SPLIT_OFF_CONFIRM?0:1;
 if(first==1)p->selected=1;
 bool direction=f->pressed[IB_GUI_UP]||f->pressed[IB_GUI_DOWN]
  ||f->pressed[IB_GUI_LEFT]||f->pressed[IB_GUI_RIGHT];
 bool pointer_owns=gutil_menu_pointer_focus(&p->focus,p->pointer_available,
  p->pointer_x,p->pointer_y,direction);
 p->pointer_target=-1;
 if(p->pointer_available)for(int k=first;k<2;k++) {
  struct menu_rect r=split_modal_button(w,h,k);
  struct gutil_button_bounds b={r.x,r.y,r.w,r.h};
  if(gutil_button_hit(&b,p->pointer_x,p->pointer_y)) {
   p->pointer_target=k;
   if(pointer_owns) {select_row(p,k,2);p->modal_origin=GUTIL_MENU_ORIGIN_POINTER;}
   break;
  }
 }
 if(phase==SCREEN_SPLIT_OFF_CONFIRM&&direction) {
  select_row(p,(f->pressed[IB_GUI_RIGHT]||f->pressed[IB_GUI_DOWN])?1:0,2);
  p->modal_origin=GUTIL_MENU_ORIGIN_DIRECTIONAL;
 }
 bool click=f->pressed[IB_GUI_CLICK];
 struct gutil_menu_decision d=gutil_menu_activate_decide(
  (struct gutil_menu_activate_state){
   .pressed=click,.navigated=direction,.pointer_valid=p->pointer_available,
   .selection_origin=p->modal_origin,.hovered=p->pointer_target,
   .hovered_enabled=p->pointer_target>=first,.selected=p->selected,
   .selected_enabled=p->selected>=first,
   /* Turn off ends Player 2's session: destructive (R-D) and a destructive
    * confirmation (Astra D2-destructive ruling B). */
   .selected_destructive=phase==SCREEN_SPLIT_OFF_CONFIRM&&p->selected==1,
   .destructive_dialog=phase==SCREEN_SPLIT_OFF_CONFIRM});
 p->selected=d.selected;p->modal_origin=d.selection_origin;
 if(d.action==GUTIL_MENU_ACTIVATE) {
  if(d.target>=first&&d.target<2) {
   p->selected=d.target;
   if(phase==SCREEN_SPLIT_OFF_CONFIRM) {
    if(d.target==0)screen_options_split_off_cancel();
    else screen_options_split_off_confirm(&gstate.players[1]);
   } else (void)screen_options_split_off_retry(&gstate.players[1]);
   audio_menu_event(MENU_SOUND_SELECT,time_get());
  }
  return true;
 }
 if(phase==SCREEN_SPLIT_OFF_CONFIRM&&(f->pressed[IB_HOME]
   ||f->pressed[IB_GUI_CLICK_ALT]||f->pressed[IB_INVENTORY])) {
  screen_options_split_off_cancel();p->selected=0;
  audio_menu_event(MENU_SOUND_BACK,time_get());
 }
 return true;
}
enum local_pause_result local_pause_update(struct local_pause* p,struct client_session* s,
 const struct input_player_frame* f,int x,int y,int w,int h) {
 if(!p||!p->open||!f||!f->connected)return LOCAL_PAUSE_NONE;
 if(p->page==LOCAL_PAUSE_ACHIEVEMENTS) {
  /* Input pixels use main's screen-space pane; painting uses its pane-local
   * projection. Preserve the caller's frame and translate only pixel IR. */
  struct input_player_frame child=*f;
  if(!child.pointer_normalized) {child.pointer_x-=x;child.pointer_y-=y;}
  enum achievements_action action=screen_achievements_update_owned(
   screen_achievements_owner(p->host?0:1),&child,0,0,w,h);
  if(action==ACHIEVEMENTS_RESUME) {
   p->open=false;audio_menu_event(MENU_SOUND_BACK,time_get());return LOCAL_PAUSE_BACK;
  }
  if(action==ACHIEVEMENTS_BACK) {
   p->page=LOCAL_PAUSE_MENU;p->selected=1;
   p->focus=(struct gutil_menu_focus){.initialized=true};
   p->selection_origin=GUTIL_MENU_ORIGIN_DEFAULT;
  }
  return LOCAL_PAUSE_NONE;
 }
 if(!p->host&&p->exit_pending&&s) {
  if(s->local_phase==CLIENT_GUEST_FAILED) {p->exit_pending=false;p->exit_failed=true;p->selected=3;}
 }
 int rows=pause_rows(p);
 struct menu_composition c;local_pause_compose(&c,0,0,w,h,0,rows);p->pointer_target=-1;
 float px,py,a;p->pointer_available=input_owner_pointer(f,x,y,w,h,&px,&py,&a);
 if(p->pointer_available) {p->pointer_x=px;p->pointer_y=py;p->pointer_angle=a;}
 if(p->page==LOCAL_PAUSE_OPTIONS) {
  if(screen_options_split_off_rearm_step(frame_active(f)))return LOCAL_PAUSE_NONE;
  if(split_modal_update(p,f,w,h))return LOCAL_PAUSE_NONE;
 }
 bool direction=f->pressed[IB_GUI_UP]||f->pressed[IB_GUI_DOWN]||f->pressed[IB_GUI_LEFT]||f->pressed[IB_GUI_RIGHT];
 bool pointer_owns=gutil_menu_pointer_focus(&p->focus,p->pointer_available,px,py,direction);
 if(p->pointer_available) {
  for(int k=0;k<rows;k++) {
   struct gutil_button_bounds b={c.actions[k].x,c.actions[k].y,c.actions[k].w,c.actions[k].h};
   if(gutil_button_hit(&b,p->pointer_x,p->pointer_y)) {
    p->pointer_target=k;
    if(pointer_owns&&!p->exit_pending) {
     select_row(p,k,rows);p->selection_origin=GUTIL_MENU_ORIGIN_POINTER;
    }
    break;
   }
  }
 }
 if(p->exit_pending)return LOCAL_PAUSE_NONE;
 bool grid=p->page==LOCAL_PAUSE_MENU&&h<480;
 if(f->pressed[IB_GUI_UP]) {
  select_row(p,p->selected-(grid?2:1),rows);
  p->selection_origin=GUTIL_MENU_ORIGIN_DIRECTIONAL;
 }
 if(f->pressed[IB_GUI_DOWN]) {
  select_row(p,p->selected+(grid?2:1),rows);
  p->selection_origin=GUTIL_MENU_ORIGIN_DIRECTIONAL;
 }
 if(grid) {
  if(f->pressed[IB_GUI_LEFT]&&(p->selected%2))select_row(p,p->selected-1,rows);
  if(f->pressed[IB_GUI_RIGHT]&&!(p->selected%2))select_row(p,p->selected+1,rows);
  if(f->pressed[IB_GUI_LEFT]||f->pressed[IB_GUI_RIGHT])
   p->selection_origin=GUTIL_MENU_ORIGIN_DIRECTIONAL;
 }
 bool click=f->pressed[IB_GUI_CLICK];
 struct gutil_menu_decision d=gutil_menu_activate_decide(
  (struct gutil_menu_activate_state){
   .pressed=click,.navigated=direction,.pointer_valid=p->pointer_available,
   .selection_origin=p->selection_origin,.hovered=p->pointer_target,
   .hovered_enabled=p->pointer_target>=0&&p->pointer_target<rows
    &&(p->page!=LOCAL_PAUSE_MENU||p->host||p->pointer_target!=2),
   .selected=p->selected,.selected_enabled=p->selected>=0&&p->selected<rows
    &&(p->page!=LOCAL_PAUSE_MENU||p->host||p->selected!=2),
   /* "Confirm save and quit" ends the session (and Player 2's with it): a
    * destructive confirmation (Astra D2-destructive ruling B); Cancel is its
    * safe default. */
   .selected_destructive=p->page==LOCAL_PAUSE_HOST_CONFIRM&&p->selected==1,
   .destructive_dialog=p->page==LOCAL_PAUSE_HOST_CONFIRM});
 if(d.action==GUTIL_MENU_FOCUS_ONLY)select_row(p,d.selected,rows);
 else p->selected=d.selected;
 p->selection_origin=d.selection_origin;
 if(d.action==GUTIL_MENU_ACTIVATE) {
  p->selected=d.target;return activate(p,s);
 }
 if(click)return LOCAL_PAUSE_NONE;
 /* IB_INVENTORY is the CLOSE role (Remote 1 / Classic X / GameCube Y). This
  * screen used to IGNORE it, so the one button the rest of the UI closes with
  * did nothing in the only menu a split guest can open -- Astra menu review
  * 2026-09-19 MUST item 8, settled by the maintainer the same day: "Close =
  * Back / Resume, consistently; quitting only through the named menu row."
  * go_back() is the same Back the host confirmation page uses, so Close steps
  * back one page there rather than confirming anything, and the exit_pending
  * guard above keeps it inert while a save is in flight. */
 if(f->pressed[IB_HOME]||f->pressed[IB_GUI_CLICK_ALT]||f->pressed[IB_INVENTORY])
  return go_back(p);
 return LOCAL_PAUSE_NONE;
}
void local_pause_host_exit_settled(struct local_pause* p,bool accepted) {
 if(!p||!p->open||!p->host||!p->exit_pending)return;
 p->exit_pending=false;
 if(accepted) {p->open=false;audio_menu_event(MENU_SOUND_BACK,time_get());}
 else {p->page=LOCAL_PAUSE_MENU;p->selected=3;p->exit_failed=true;}
}
static void panel(const struct menu_rect* r) {
 gfx_texture(false);gutil_texquad_col(r->x-2,r->y-2,0,0,0,0,r->w+4,r->h+4,17,17,17,255);
 gfx_texture(true);gfx_bind_texture(&texture_gui_crafting);
 for(int yy=0;yy<r->h;yy+=32)for(int xx=0;xx<r->w;xx+=32) {
  int w=r->w-xx<32?r->w-xx:32,h=r->h-yy<32?r->h-yy:32;
  gutil_texquad_col(r->x+xx,r->y+yy,150,62,(w+1)/2,(h+1)/2,w,h,255,255,255,255);
 }
 gfx_texture(false);
 gutil_texquad_col(r->x,r->y,0,0,0,0,r->w,3,255,255,255,153);
 gutil_texquad_col(r->x,r->y,0,0,0,0,3,r->h,255,255,255,153);
 gutil_texquad_col(r->x,r->y+r->h-3,0,0,0,0,r->w,3,85,85,85,255);
 gutil_texquad_col(r->x+r->w-3,r->y,0,0,0,0,3,r->h,85,85,85,255);gfx_texture(true);
}
static void button(const struct menu_rect* r,const char* label,bool hover,bool enabled) {
 int half=r->w/GFX_GUI_SCALE/2,ty=hover&&enabled?86:66;uint8_t alpha=enabled?255:150;
 gfx_bind_texture(&texture_gui_widgets);
 gutil_texquad_col(r->x,r->y,0,ty,half,20,r->w/2,r->h,255,255,255,alpha);
 gutil_texquad_col(r->x+r->w/2,r->y,200-half,ty,half,20,r->w/2,r->h,255,255,255,alpha);
 int scale=16,tw=gutil_font_width(label,scale);uint8_t ink=enabled?(hover?255:224):160;
 gutil_text_rgb(r->x+(r->w-tw)/2,r->y+(r->h-scale)/2,label,scale,true,ink,ink,enabled&&hover?224:ink);
}
static const char* pause_status(const struct local_pause* p) {
 if(p->page==LOCAL_PAUSE_OPTIONS)return "Host setting - world continues";
 if(p->exit_pending)return p->host?"World continues - saving...":"World continues - leaving...";
 if(p->exit_failed)return p->host?"World continues - save failed; retry":"World continues - leave failed; retry";
 return "World continues";
}
void local_pause_render(const struct local_pause* p,unsigned player,const struct input_player_frame* f,int x,int y,int w,int h) {
 if(!p||!p->open||w<=0||h<=0)return;
 if(p->page==LOCAL_PAUSE_ACHIEVEMENTS) {
  screen_achievements_render_owned(screen_achievements_owner(player),f,x,y,w,h);
  return;
 }
 gfx_texture(false);gutil_texquad_col(x,y,0,0,0,0,w,h,0,0,0,180);gfx_texture(true);
 int rows=pause_rows(p);
 struct menu_composition c;local_pause_compose(&c,x,y,w,h,gutil_connection_notice_width(player,true,true),rows);
 bool full=c.layout==MENU_SCREEN_FULL;if(!full)panel(&c.window);
 struct menu_rect head=full?c.header:menu_composition_header_content(&c);
 const char* title=p->page==LOCAL_PAUSE_HOST_CONFIRM?"Exit game?"
  :p->page==LOCAL_PAUSE_OPTIONS?"Options":"Game menu";
 int ts=full?26:16,tx=full?head.x+(head.w-gutil_font_width(title,ts))/2:head.x;
 if(full)gutil_text(tx,head.y+(head.h-ts)/2,title,ts,true);
 else gutil_text_rgb(tx,head.y+(head.h-ts)/2,title,ts,false,64,64,64);
 for(int k=0;k<rows;k++)button(&c.actions[k],pause_label(p,k),p->selected==k,
  !p->exit_pending&&(p->page!=LOCAL_PAUSE_MENU||p->host||k!=2));
 struct menu_rect st=full?menu_pause_status(&c,rows)
  :(struct menu_rect){x,y+176,w,20};
 enum input_category cat=f?f->category:INPUT_CAT_NONE;const char* name=controller_family_name(cat);
 enum controller_picture pic=controller_picture_of((struct input_device){0},cat);
 if(full) {
  /* Picture + name centred as ONE group (maintainer 10-31): the old fixed
   * offsets left the pair off-centre for short or long family names. */
  int pw=gutil_controller_picture_width(pic,64),nw=gutil_font_width(name,14);
  int gx=st.x+(st.w-(pw+8+nw))/2;
  gutil_controller_picture(gx,st.y,pw,64,pic);
  gutil_text(gx+pw+8,st.y+25,name,14,true);
 } else {
  /* Approved split status: a 20 px text-only controller-name row, no
   * picture (menus.html:80/85/901, Astra ui1). */
  (void)pic;
 }
 const char* state=pause_status(p);
 if(full) gutil_text_rgb(x+(w-gutil_font_width(state,14))/2,st.y+26,state,14,false,64,64,64);
 else {
  /* split: the hint row's strip (pane 196-224) takes the old second line,
   * so the state joins the controller name on the one 20 px status line. */
  char line[96];snprintf(line,sizeof line,"%s - %s",name,state);
  gutil_text_rgb(st.x+(st.w-gutil_font_width(line,14))/2,st.y+3,line,14,false,64,64,64);
 }
 if(f) {
  /* The centred hint row (controller UI redesign 10-31, A1 helper). */
  static const struct gutil_hint_item items[]={
   {{IB_GUI_UP},1,"Move",0,false},{{IB_GUI_CLICK},1,"Select",1,false},
   {{IB_HOME},1,"Resume",2,true}};
  struct gutil_hint_row row;
  if(gutil_hint_row_layout(f,x,y,w,h,!full,GUTIL_HINT_ALIGN_CENTRE,items,3,&row))
   gutil_hint_row_draw(&row);
 }
 if(c.notice_in_header)gutil_connection_notices_in_header(player,true,c.notice.x,c.notice.y,c.notice.w,c.notice.h);
 /* gutil_connection_notices takes the EXPANDED rectangle (x-8,y-8,w+16,h+16),
  * the same adapter screen_render2D_notices applies; passing the bare
  * (24,8) x 28 rect left 12 px and suppressed the card (Astra ui1). */
 else gutil_connection_notices(player,true,c.notice.x-8,c.notice.y-8,c.notice.w+16,c.notice.h+16);
 enum screen_options_split_off_phase phase=screen_options_split_off_phase();
 if(p->page==LOCAL_PAUSE_OPTIONS&&phase!=SCREEN_SPLIT_OFF_IDLE) {
  gfx_texture(false);gutil_texquad_col(x,y,0,0,0,0,w,h,0,0,0,180);gfx_texture(true);
  struct menu_rect modal=split_modal_panel(w,h);modal.x+=x;modal.y+=y;panel(&modal);
  const char* body="Player 2 will leave the local session.";
  if(phase==SCREEN_SPLIT_OFF_LEAVE_PENDING)body="Waiting for Player 2 to leave...";
  else if(phase==SCREEN_SPLIT_OFF_LEAVE_FAILED)body="Player 2 could not leave. Retry to continue.";
  else if(phase==SCREEN_SPLIT_OFF_REQUEST_PENDING)body="Player 2 left. Turning split-screen off...";
  else if(phase==SCREEN_SPLIT_OFF_REQUEST_FAILED)body="Player 2 left. The server did not confirm Off.";
  int title_size=h>=480?23:18,body_size=h>=480?15:14;
  gutil_text_rgb(modal.x+24,modal.y+20,"Turn off split-screen?",title_size,false,32,32,32);
  gutil_text_rgb(modal.x+24,modal.y+58,body,body_size,false,32,32,32);
  if(phase==SCREEN_SPLIT_OFF_CONFIRM)for(int k=0;k<2;k++) {
   struct menu_rect r=split_modal_button(w,h,k);r.x+=x;r.y+=y;
   button(&r,k?"Turn off":"Keep on",p->selected==k,true);
  } else if(phase==SCREEN_SPLIT_OFF_LEAVE_FAILED
    ||phase==SCREEN_SPLIT_OFF_REQUEST_FAILED) {
   struct menu_rect r=split_modal_button(w,h,1);r.x+=x;r.y+=y;
   button(&r,phase==SCREEN_SPLIT_OFF_LEAVE_FAILED?"Retry leave":"Retry turn off",true,true);
  }
 }
 if(p->pointer_available) {
  gfx_bind_texture(&texture_pointer);
  gutil_texquad_rt_any(p->pointer_x+x,p->pointer_y+y,glm_rad(p->pointer_angle),0,0,256,256,48*GFX_GUI_SCALE,48*GFX_GUI_SCALE);
 }
}
