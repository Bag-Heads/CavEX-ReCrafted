/* Owner-local Pause through the real session lifecycle. GPL-3.0-or-later. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "game/client_session.h"
#include "game/gui/menu_header.h"
#include "game/game_state.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "platform/input_owner.h"
#include "platform/menu_sound.h"
#include "block/blocks.h"
#include "item/items.h"
#include "world.h"
#include "particle.h"
#include "game/gui/screen.h"
static unsigned back_cues;
void __wrap_audio_menu_event(enum menu_sound_event event,ptime_t now) {(void)now;if(event==MENU_SOUND_BACK)back_cues++;}
static enum local_pause_result press(struct client_session* s,int button) {
 struct input_player_frame f={.connected=true,.category=INPUT_CAT_GAMECUBE};
 if(button>=0)f.pressed[button]=true;
 return local_pause_update(&s->pause,s,&f,0,240,640,240);
}
static void test_host(void) {
 struct client_session s={0};struct game_state before=gstate;
 assert(local_pause_open(&s.pause,true));assert(press(&s,IB_HOME)==LOCAL_PAUSE_BACK&&!s.pause.open);
 assert(local_pause_open(&s.pause,true));assert(press(&s,IB_GUI_CLICK_ALT)==LOCAL_PAUSE_BACK&&!s.pause.open);
 assert(local_pause_open(&s.pause,true));press(&s,IB_GUI_DOWN);press(&s,IB_GUI_RIGHT);press(&s,IB_GUI_CLICK);
 assert(s.pause.page==LOCAL_PAUSE_HOST_CONFIRM&&s.pause.selected==0);
 struct input_player_frame both={.connected=true};both.pressed[IB_GUI_CLICK]=both.pressed[IB_HOME]=true;
 assert(local_pause_update(&s.pause,&s,&both,0,0,640,240)==LOCAL_PAUSE_NONE);
 assert(s.pause.open&&s.pause.page==LOCAL_PAUSE_MENU&&s.pause.selected==3);
 press(&s,IB_GUI_CLICK);press(&s,IB_GUI_DOWN);
 assert(press(&s,IB_GUI_CLICK)==LOCAL_PAUSE_HOST_EXIT&&s.pause.exit_pending);
 assert(press(&s,IB_GUI_CLICK)==LOCAL_PAUSE_NONE);
 local_pause_host_exit_settled(&s.pause,false);
 assert(s.pause.open&&s.pause.exit_failed&&!s.pause.exit_pending);
 press(&s,IB_GUI_CLICK);press(&s,IB_GUI_DOWN);assert(press(&s,IB_GUI_CLICK)==LOCAL_PAUSE_HOST_EXIT);
 local_pause_host_exit_settled(&s.pause,true);assert(!s.pause.open);
 assert(!memcmp(&gstate,&before,sizeof before));
 puts("PASS local Pause host: Home back with B alias, explicit Cancel-first confirmation, no duplicate exit, rejected enqueue retains retry, no global pause mutation");
}
/* Astra menu review 2026-09-19 MUST item 8 + the maintainer's decision the same
 * day ("Close = Back / Resume, consistently; quitting only through the named
 * menu row"). The owner-local Pause IGNORED the Close role (Remote 1 / Classic
 * X / GameCube Y) entirely: the one button the rest of the UI closes with did
 * nothing in the only menu a split guest can open. It is Back here now -- and
 * it must never become a second, unconfirmed route to Save and quit. */
static void test_close_is_back(void) {
 struct client_session s={0};struct game_state before=gstate;
 assert(local_pause_open(&s.pause,true));
 assert(press(&s,IB_INVENTORY)==LOCAL_PAUSE_BACK&&!s.pause.open);
 /* On the host confirmation page it steps BACK to the menu, exactly like HOME,
  * and never confirms the exit. */
 assert(local_pause_open(&s.pause,true));press(&s,IB_GUI_DOWN);press(&s,IB_GUI_RIGHT);press(&s,IB_GUI_CLICK);
 assert(s.pause.page==LOCAL_PAUSE_HOST_CONFIRM);
 assert(press(&s,IB_INVENTORY)==LOCAL_PAUSE_NONE);
 assert(s.pause.open&&s.pause.page==LOCAL_PAUSE_MENU&&s.pause.selected==3&&!s.pause.exit_pending);
 /* ... and the named row still quits, through its own confirmation. */
 press(&s,IB_GUI_CLICK);press(&s,IB_GUI_DOWN);
 assert(press(&s,IB_GUI_CLICK)==LOCAL_PAUSE_HOST_EXIT&&s.pause.exit_pending);
 /* Frozen while the save is in flight: Close cannot close a pending pause. */
 assert(press(&s,IB_INVENTORY)==LOCAL_PAUSE_NONE&&s.pause.open&&s.pause.exit_pending);
 local_pause_host_exit_settled(&s.pause,true);assert(!s.pause.open);
 /* GUEST: Close leaves the PAUSE, never the session. */
 struct client_session g={.local_ticket=3,.local_generation=4,.local_bound=true,
  .local_phase=CLIENT_GUEST_ATTACHED};
 assert(local_pause_open(&g.pause,false));
 assert(press(&g,IB_INVENTORY)==LOCAL_PAUSE_BACK&&!g.pause.open);
 assert(g.local_phase==CLIENT_GUEST_ATTACHED&&!g.local_pending
  &&"the guest's Close requested a leave");
 assert(!memcmp(&gstate,&before,sizeof before));
 puts("PASS local Pause Close: Back on the menu page, back to the menu from the host confirmation, never an exit for host or guest, inert while one is pending");
}
static void test_guest(void) {
 struct client_session s={.local_ticket=7,.local_generation=11,.local_bound=true,.local_phase=CLIENT_GUEST_ATTACHED};
 s.container.view=1;s.menu_host.mode=LOCAL_MENU_CONTAINER;
 struct container_compact retained=s.container;
 assert(input_player_bind(1,(struct input_device){INPUT_DEVICE_KEYBOARD,0}));
 assert(local_pause_open(&s.pause,false));press(&s,IB_GUI_DOWN);press(&s,IB_GUI_RIGHT);press(&s,IB_GUI_CLICK);
 assert(s.pause.open&&s.pause.exit_pending&&s.local_phase==CLIENT_GUEST_LEAVING&&s.local_pending);
 struct client_rpc reply={.type=CRPC_LOCAL_GUEST,.payload.local_guest={.ticket=7,.generation=10,.result=LOCAL_GUEST_LEFT}};
 client_session_local_status(&s,&reply);assert(s.local_bound&&s.pause.open);
 reply.payload.local_guest.generation=11;reply.payload.local_guest.result=LOCAL_GUEST_LEAVE_FAILED;
 client_session_local_status(&s,&reply);press(&s,-1);
 assert(s.pause.open&&s.pause.exit_failed&&!s.pause.exit_pending);
 assert(input_player_frame(1));
 assert(s.menu_host.mode==LOCAL_MENU_CONTAINER&&!memcmp(&s.container,&retained,sizeof retained));
 press(&s,IB_GUI_CLICK);assert(s.pause.exit_pending&&s.local_phase==CLIENT_GUEST_LEAVING);
 unsigned before=back_cues;reply.payload.local_guest.result=LOCAL_GUEST_LEFT;
 client_session_local_status(&s,&reply);
 assert(!s.local_bound&&s.local_phase==CLIENT_GUEST_IDLE&&!s.pause.open&&back_cues==before+1);
 client_session_local_status(&s,&reply);assert(back_cues==before+1);
 assert(!input_player_frame(1));
 assert(input_player_bind(1,(struct input_device){INPUT_DEVICE_KEYBOARD,0}));
 input_player_unbind(1);
 puts("PASS local Pause guest: real leave request, stale ACK rejection, failed-save retry, exact matching LEFT retires owner and cues once");
}
static void test_pointer(void) {
 struct client_session s={0};assert(local_pause_open(&s.pause,true));
 struct menu_composition c;local_pause_compose(&c,0,0,640,240,80,4);
 assert(c.action_count==4&&c.notice_in_header);
 for(size_t k=0;k<c.action_count;k++)assert(menu_rect_contains(&c.view,&c.actions[k]));
 struct input_player_frame f={.connected=true,.pointer_valid=true,.pointer_x=c.actions[3].x+16,.pointer_y=240+c.actions[3].y+8};
 local_pause_update(&s.pause,&s,&f,0,240,640,240);f.pointer_x+=8;
 local_pause_update(&s.pause,&s,&f,0,240,640,240);
 assert(s.pause.pointer_y==c.actions[3].y+8&&s.pause.pointer_target==3);
 f.pressed[IB_GUI_CLICK]=true;local_pause_update(&s.pause,&s,&f,0,240,640,240);
 assert(s.pause.page==LOCAL_PAUSE_HOST_CONFIRM&&s.pause.selected==0);
 /* A stationary pointer yields to explicit directional input. The click is
  * still present in this synthetic frame, so navigation consumes it; a later
  * activation confirms the newly focused row. */
 f.pressed[IB_GUI_DOWN]=true;local_pause_update(&s.pause,&s,&f,0,240,640,240);
 assert(s.pause.selected==1&&!s.pause.exit_pending);
 f.pressed[IB_GUI_DOWN]=false;local_pause_update(&s.pause,&s,&f,0,240,640,240);
 assert(s.pause.exit_pending);
 puts("PASS local Pause pointer: lower viewport coordinates, retained pointer yields to D-pad");
}
/* card 3 (2026-09-16): Save and quit is a request. The PRODUCTION client RPC
 * handlers settle the split host's owner-local Pause: RETURN_INGAME -> failed
 * in place (retry), WORLD_RESET(local_entity 0) -> pause closed AND title;
 * a nonzero WORLD_RESET (dimension switch) does not complete the exit; replies
 * that arrive with nothing pending leave the pause untouched. */
static void test_host_exit_rpc_settle(void) {
 assert(clin_init());assert(svin_init());blocks_init();items_init();
 memset(&gstate,0,sizeof gstate);world_create(&gstate.world);dict_entity_init(gstate.entities);particle_init();
 struct client_session* h=&gstate.players[0];
 h->current_screen=&screen_ingame;gstate.world_loaded=true;
 assert(local_pause_open(&h->pause,true));press(h,IB_GUI_DOWN);press(h,IB_GUI_RIGHT);press(h,IB_GUI_CLICK);
 assert(h->pause.page==LOCAL_PAUSE_HOST_CONFIRM);press(h,IB_GUI_DOWN);
 assert(press(h,IB_GUI_CLICK)==LOCAL_PAUSE_HOST_EXIT&&h->pause.exit_pending);
 /* duplicate input while pending changes nothing */
 assert(press(h,IB_GUI_CLICK)==LOCAL_PAUSE_NONE&&h->pause.exit_pending&&h->pause.page==LOCAL_PAUSE_HOST_CONFIRM);
 assert(press(h,IB_HOME)==LOCAL_PAUSE_NONE&&h->pause.open&&h->pause.exit_pending);
 /* a nonzero WORLD_RESET is a dimension switch, not the unload: it never reaches the
  * title, and (card 4) the already-pending host exit survives it, still frozen. */
 struct client_rpc reset5={.type=CRPC_WORLD_RESET,.payload.world_reset={.dimension=WORLD_DIM_OVERWORLD,.local_entity=5}};
 clin_process(&reset5);
 assert(h->current_screen!=&screen_title&&h->pause.open&&h->pause.exit_pending&&!h->pause.exit_failed);
 /* refused save: RETURN_INGAME -> failed in place, pause up, retry available, never the title */
 struct client_rpc ret={.type=CRPC_RETURN_INGAME};clin_process(&ret);
 assert(h->pause.open&&h->pause.exit_failed&&!h->pause.exit_pending&&h->current_screen!=&screen_title);
 /* nothing pending: an unrelated RETURN leaves the pause byte-identical */
 struct local_pause snap=h->pause;clin_process(&ret);assert(!memcmp(&snap,&h->pause,sizeof snap));
 /* retry -> pending again -> the real unload's WORLD_RESET(0) closes the pause and selects the title */
 press(h,IB_GUI_CLICK);press(h,IB_GUI_DOWN);assert(press(h,IB_GUI_CLICK)==LOCAL_PAUSE_HOST_EXIT&&h->pause.exit_pending);
 struct client_rpc reset0={.type=CRPC_WORLD_RESET,.payload.world_reset={.dimension=WORLD_DIM_OVERWORLD,.local_entity=0}};
 clin_process(&reset0);
 assert(!h->pause.open&&!h->pause.exit_pending&&!h->pause.exit_failed&&h->current_screen==&screen_title);
 /* settled: a late RETURN or RESET(0) with nothing pending changes nothing */
 snap=h->pause;clin_process(&ret);assert(!memcmp(&snap,&h->pause,sizeof snap));
 puts("PASS local Pause host via production RPC handlers: pending survives duplicate input and a dimension reset (never the title), RETURN fails in place with retry, RESET0 closes and reaches title, idle replies inert");
}
/* card 4 (2026-09-16, Astra #9): a pending split-host Save and quit must SURVIVE a
 * dimension-switch WORLD_RESET (nonzero local_entity) -- the queued unload still
 * answers with WORLD_RESET(0) or RETURN_INGAME and client_interface settles this
 * pause on either. Before the fix client_session_clear_windows wiped the pause and
 * the host ended on the loading screen with no world (probe-dimension-reset.log). */
static void arm_host_exit(struct client_session* h) {
 h->current_screen=&screen_ingame;gstate.world_loaded=true;
 assert(local_pause_open(&h->pause,true));press(h,IB_GUI_DOWN);press(h,IB_GUI_RIGHT);press(h,IB_GUI_CLICK);press(h,IB_GUI_DOWN);
 assert(press(h,IB_GUI_CLICK)==LOCAL_PAUSE_HOST_EXIT&&h->pause.exit_pending);
}
static void dimension_reset(void) {
 struct client_rpc r={.type=CRPC_WORLD_RESET,.payload.world_reset={.dimension=WORLD_DIM_NETHER,.local_entity=5}};
 clin_process(&r);
}
static void unload_reset(void) {
 struct client_rpc r={.type=CRPC_WORLD_RESET,.payload.world_reset={.dimension=WORLD_DIM_OVERWORLD,.local_entity=0}};
 clin_process(&r);
}
static void test_pending_exit_survives_dimension_reset(void) {
 struct client_session* h=&gstate.players[0];struct client_session* g=&gstate.players[1];
 /* (A) pending -> nonzero RESET -> RESET0. Alongside: a pending GUEST pause, a host
  * pause's other windows, and (in C below) a non-pending host pause are all cleared. */
 arm_host_exit(h);
 g->pause=(struct local_pause){.open=true,.host=false,.exit_pending=true,.selected=1};
 h->menu_host.mode=LOCAL_MENU_CONTAINER;h->container.view=1;h->visible_window=3;
 dimension_reset();
 assert(h->pause.open&&h->pause.host&&h->pause.exit_pending&&!h->pause.exit_failed);   /* survives */
 assert(!g->pause.open&&!g->pause.exit_pending);                                        /* guest pause cleared */
 assert(h->menu_host.mode==LOCAL_MENU_NONE&&h->container.view==0&&h->visible_window==-1); /* other UI cleared */
 assert(h->current_screen!=&screen_title);
 /* frozen: duplicate confirm and HOME are inert while pending */
 assert(press(h,IB_GUI_CLICK)==LOCAL_PAUSE_NONE&&h->pause.exit_pending&&h->pause.open);
 assert(press(h,IB_HOME)==LOCAL_PAUSE_NONE&&h->pause.exit_pending&&h->pause.open);
 unload_reset();
 assert(!h->pause.open&&!h->pause.exit_pending&&h->current_screen==&screen_title);
 /* (B) pending -> nonzero RESET -> RETURN_INGAME: failure visible in the open pause; retry -> RESET0 -> title */
 arm_host_exit(h);dimension_reset();
 assert(h->pause.open&&h->pause.exit_pending);
 struct client_rpc ret={.type=CRPC_RETURN_INGAME};clin_process(&ret);
 assert(h->pause.open&&h->pause.exit_failed&&!h->pause.exit_pending&&h->current_screen!=&screen_title);
 press(h,IB_GUI_CLICK);press(h,IB_GUI_DOWN);assert(press(h,IB_GUI_CLICK)==LOCAL_PAUSE_HOST_EXIT&&h->pause.exit_pending);
 unload_reset();
 assert(!h->pause.open&&h->current_screen==&screen_title);
 /* (C) a host pause WITHOUT a pending exit is still cleared by the same reset */
 h->current_screen=&screen_ingame;gstate.world_loaded=true;
 assert(local_pause_open(&h->pause,true));press(h,IB_GUI_DOWN);press(h,IB_GUI_RIGHT);press(h,IB_GUI_CLICK);
 assert(h->pause.open&&h->pause.page==LOCAL_PAUSE_HOST_CONFIRM&&!h->pause.exit_pending);
 dimension_reset();
 assert(!h->pause.open&&!h->pause.exit_pending&&h->current_screen!=&screen_title);
 puts("PASS local Pause host across a dimension reset: pending exit survives frozen and still reaches title or fails in place with retry; guest pause, other windows and a non-pending host pause are cleared");
}
static void test_host_options_route(void) {
 struct client_session host={0},guest={.local_phase=CLIENT_GUEST_ATTACHED,
  .local_bound=true,.local_gameplay_ready=true};
 assert(local_pause_open(&host.pause,true));
 struct input_player_frame both={.connected=true,.category=INPUT_CAT_GAMECUBE};
 both.pressed[IB_GUI_DOWN]=true;both.pressed[IB_GUI_CLICK]=true;
 local_pause_update(&host.pause,&host,&both,0,240,640,240);
 assert(host.pause.page==LOCAL_PAUSE_MENU&&host.pause.selected==2
  &&"outer navigation and activation on one frame must not activate");
 struct menu_composition c;local_pause_compose(&c,0,0,640,240,0,4);
 struct input_player_frame pointer={.connected=true,.category=INPUT_CAT_GAMECUBE,
  .pointer_valid=true,.pointer_x=c.actions[1].x+16,.pointer_y=240+c.actions[1].y+8};
 local_pause_update(&host.pause,&host,&pointer,0,240,640,240);
 pointer.pointer_x+=16;
 local_pause_update(&host.pause,&host,&pointer,0,240,640,240);
 assert(host.pause.selected==1);
 pointer.pressed[IB_GUI_LEFT]=true;
 local_pause_update(&host.pause,&host,&pointer,0,240,640,240);
 pointer.pressed[IB_GUI_LEFT]=false;pointer.pressed[IB_GUI_DOWN]=true;
 local_pause_update(&host.pause,&host,&pointer,0,240,640,240);
 assert(host.pause.selected==2);
 pointer.pressed[IB_GUI_DOWN]=false;pointer.pressed[IB_GUI_CLICK]=true;
 local_pause_update(&host.pause,&host,&pointer,0,240,640,240);
 assert(host.pause.page==LOCAL_PAUSE_OPTIONS&&host.pause.selected==0
  &&"the directional Options cell must beat a resting pointer");
 assert(guest.local_phase==CLIENT_GUEST_ATTACHED&&guest.local_bound);
 press(&host,IB_HOME);
 assert(host.pause.page==LOCAL_PAUSE_MENU&&host.pause.selected==2);
 press(&host,IB_GUI_RIGHT);assert(host.pause.selected==3);
 assert(press(&host,IB_INVENTORY)==LOCAL_PAUSE_BACK&&!host.pause.open);
 assert(local_pause_open(&guest.pause,false));
 press(&guest,IB_GUI_DOWN);press(&guest,IB_GUI_CLICK);
 assert(guest.pause.selected==2&&guest.pause.page==LOCAL_PAUSE_MENU
  &&guest.local_phase==CLIENT_GUEST_ATTACHED&&!guest.pause.exit_pending);
 local_pause_reset(&guest.pause);
 puts("PASS local Pause grid Options: host-only cell 2/back/focus, guest disabled Options inert, exit cell 3");
}
/* Astra D2-destructive ruling B (tmp/jobs/next-gaps-0924/astra-D2-destructive.raw):
 * "Confirm save and quit" ends the session, so the host confirmation page is a
 * destructive confirmation. A valid sprite resting on one row while the ring (a
 * direction press) sits on the other: the first A only moves focus under the
 * sprite and executes neither row; holding A does nothing; the next FRESH A,
 * focus reconciled, answers the hovered row. Both directions, stationary and
 * reappearing IR. */
static void test_host_confirm_reconciles_focus(void) {
 static const struct {int sprite,ring;bool reappear;} rows[]={
  {0,1,false},{0,1,true},{1,0,false},{1,0,true}};
 struct menu_composition c;local_pause_compose(&c,0,0,640,240,0,2);
 for(size_t k=0;k<sizeof rows/sizeof rows[0];k++) {
  struct client_session s={0};assert(local_pause_open(&s.pause,true));
  press(&s,IB_GUI_DOWN);press(&s,IB_GUI_RIGHT);press(&s,IB_GUI_CLICK);
  assert(s.pause.page==LOCAL_PAUSE_HOST_CONFIRM&&s.pause.selected==0);
  int x=rows[k].sprite,y=rows[k].ring;
  struct input_player_frame f={.connected=true,.category=INPUT_CAT_GAMECUBE,.pointer_valid=true,
   .pointer_x=c.actions[x].x+8,.pointer_y=240+c.actions[x].y+8};
  local_pause_update(&s.pause,&s,&f,0,240,640,240);
  assert(s.pause.pointer_target==x);
  f.pressed[y?IB_GUI_DOWN:IB_GUI_UP]=true;local_pause_update(&s.pause,&s,&f,0,240,640,240);
  f.pressed[IB_GUI_DOWN]=f.pressed[IB_GUI_UP]=false;
  assert(s.pause.selected==y);
  if(rows[k].reappear) {
   f.pointer_valid=false;local_pause_update(&s.pause,&s,&f,0,240,640,240);
   f.pointer_valid=true;local_pause_update(&s.pause,&s,&f,0,240,640,240);
   assert(s.pause.selected==y&&s.pause.pointer_target==x);
  }
  f.pressed[IB_GUI_CLICK]=true;
  assert(local_pause_update(&s.pause,&s,&f,0,240,640,240)==LOCAL_PAUSE_NONE);
  f.pressed[IB_GUI_CLICK]=false;
  assert(s.pause.page==LOCAL_PAUSE_HOST_CONFIRM&&s.pause.selected==x&&!s.pause.exit_pending
   &&"a destructive confirm acted on a sprite/ring mismatch (ruling B)");
  f.down[IB_GUI_CLICK]=true;
  assert(local_pause_update(&s.pause,&s,&f,0,240,640,240)==LOCAL_PAUSE_NONE);
  assert(local_pause_update(&s.pause,&s,&f,0,240,640,240)==LOCAL_PAUSE_NONE);
  assert(s.pause.page==LOCAL_PAUSE_HOST_CONFIRM&&!s.pause.exit_pending&&"holding A confirmed");
  f.down[IB_GUI_CLICK]=false;f.released[IB_GUI_CLICK]=true;
  local_pause_update(&s.pause,&s,&f,0,240,640,240);f.released[IB_GUI_CLICK]=false;
  f.pressed[IB_GUI_CLICK]=true;
  enum local_pause_result r=local_pause_update(&s.pause,&s,&f,0,240,640,240);
  if(x==1)assert(r==LOCAL_PAUSE_HOST_EXIT&&s.pause.exit_pending);
  else assert(r==LOCAL_PAUSE_NONE&&s.pause.page==LOCAL_PAUSE_MENU&&!s.pause.exit_pending);
 }
 puts("PASS host Confirm save and quit: sprite/ring mismatch only moves focus; hold inert; fresh A answers (ruling B)");
}
int main(void) {test_host();test_close_is_back();test_guest();test_pointer();test_host_options_route();test_host_exit_rpc_settle();test_host_confirm_reconciles_focus();test_pending_exit_survives_dimension_reset();return 0;}
