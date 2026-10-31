/* Real inventory neutral gate and retained owner menus. GPL-3.0-or-later. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "block/blocks.h"
#include "game/client_session.h"
#include "game/split_layout.h"
#include "platform/input_inventory.h"
#include "platform/menu_sound.h"
#include "game/gui/container_compact.h"
#include "game/gui/local_menu_host.h"
#include "graphics/gui_util.h"
#include "game/menu_autopilot.h"
static unsigned sends,back_cues;
static struct input_inventory devices;
static struct input_device device={INPUT_DEVICE_WPAD,1};
static bool bound;
bool __wrap_input_player_device(unsigned player,struct input_device* out) {
 if(player!=1||!out||!bound)return false;
 *out=device;return true;
}
const struct input_inventory* __wrap_input_devices(void) {return &devices;}
static bool capture(void* user,struct server_rpc* rpc) {(void)user;(void)rpc;sends++;return true;}
void __wrap_audio_menu_event(enum menu_sound_event event,ptime_t now) {(void)now;if(event==MENU_SOUND_BACK)back_cues++;}
static void latch(struct input_owner* owner,bool connected,bool confirm) {
 struct input_player_frame sample={.connected=connected,.category=INPUT_CAT_GAMECUBE};
 sample.down[IB_GUI_CLICK]=confirm;
 for(int b=0;b<IB_COUNT;b++)sample.glyphs[b]=-1;
 input_owner_latch(owner,&sample);
}
static void recover_mode(enum local_menu_mode mode) {
 struct client_session s={.local_generation=111,.local_ticket=222,.visible_window=-1};
 struct window_container inventory,workbench;
 assert(windowc_create(&inventory,WINDOW_TYPE_INVENTORY,INVENTORY_SIZE));
 assert(windowc_create(&workbench,WINDOW_TYPE_WORKBENCH,CRAFTING_SIZE));
 s.windows[0]=&inventory;s.windows[1]=&workbench;
 uint8_t window=mode==LOCAL_MENU_CONTAINER?0:1;s.visible_window=window?window:-1;
 assert(local_menu_host_open(&s,1,window,640,240));
 if(mode==LOCAL_MENU_MANUAL) {
  manual_grid_open(&s.manual_grid,3,640,240);s.manual_grid.owner_index=1;
  s.recipes.active=false;s.menu_host.mode=LOCAL_MENU_MANUAL;
 }
 assert(s.menu_host.mode==mode);
 struct inventory* items=windowc_get_latest(s.windows[window]);
 items->items[items->capacity-1]=(struct item_data){.id=BLOCK_DIRT,.count=17};
 items->picked_item=(struct item_data){.id=BLOCK_STONE,.count=3};
 struct container_compact compact=s.container;struct recipe_browser recipes=s.recipes;struct manual_grid manual=s.manual_grid;
 uint32_t generation=s.windows[window]->generation;
 struct input_owner owner={0};assert(input_owner_bind(&owner,1,0,(struct input_device){INPUT_DEVICE_GAMECUBE,1}));
 latch(&owner,true,false);assert(!local_menu_host_recovery_frame(&s,1,&owner.frame,0,240,640,240));
 latch(&owner,false,true);assert(local_menu_host_frame(&s,1,&owner.frame,0,240,640,240,.016F,capture,NULL));
 assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_LOST);
 latch(&owner,true,true);assert(!owner.frame.pressed[IB_GUI_CLICK]);
 assert(local_menu_host_frame(&s,1,&owner.frame,0,240,640,240,.016F,capture,NULL));
 assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_READY);
 latch(&owner,true,true);assert(!owner.frame.pressed[IB_GUI_CLICK]);
 assert(local_menu_host_frame(&s,1,&owner.frame,0,240,640,240,.016F,capture,NULL));
 latch(&owner,true,false);assert(local_menu_host_frame(&s,1,&owner.frame,0,240,640,240,.016F,capture,NULL));
 latch(&owner,true,true);assert(owner.frame.pressed[IB_GUI_CLICK]);
 assert(local_menu_host_frame(&s,1,&owner.frame,0,240,640,240,.016F,capture,NULL));
 assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_NONE&&s.menu_host.mode==mode);
 assert(s.windows[window]->generation==generation&&s.local_generation==111&&s.local_ticket==222);
 assert(items->items[items->capacity-1].count==17&&items->picked_item.count==3);
 assert(!memcmp(&s.container,&compact,sizeof compact)&&!memcmp(&s.recipes,&recipes,sizeof recipes)&&!memcmp(&s.manual_grid,&manual,sizeof manual));
 assert(!sends);windowc_destroy(&workbench);windowc_destroy(&inventory);
}
static void test_physical_neutral(void) {
 struct input_owner owners[2]={0};assert(input_owner_bind(owners,2,1,device));bound=true;
 struct input_identity identity={true,INPUT_CAT_NUNCHUK};ptime_t now={0};
 input_inventory_poll(&devices,device,identity,true,true,true,now,owners,2);
 input_inventory_poll(&devices,device,identity,true,true,true,time_add_ms(now,250),owners,2);
 const struct input_device_status* status=input_inventory_device(&devices,device);
 assert(status->published.connected&&status->armed);
 struct client_session s={0};struct input_player_frame frame={.connected=true,.category=INPUT_CAT_NUNCHUK};
 assert(!local_menu_host_recovery_frame(&s,1,&frame,0,240,640,240));
 identity.connected=false;
 input_inventory_poll(&devices,device,identity,false,false,false,time_add_ms(now,300),owners,2);
 frame.connected=false;assert(local_menu_host_recovery_frame(&s,1,&frame,0,240,640,240));
 assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_LOST);
 assert(s.menu_host.recovery.reason==CONTROLLER_EVENT_DISCONNECTED);
 input_inventory_poll(&devices,device,identity,false,false,false,time_add_ms(now,550),owners,2);
 frame.connected=false;assert(local_menu_host_recovery_frame(&s,1,&frame,0,240,640,240));
 assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_LOST);
 identity=(struct input_identity){true,INPUT_CAT_NUNCHUK};
 input_inventory_poll(&devices,device,identity,true,true,false,time_add_ms(now,600),owners,2);
 input_inventory_poll(&devices,device,identity,true,true,false,time_add_ms(now,850),owners,2);
 assert(status->published.connected&&!status->armed);
 frame.pressed[IB_GUI_CLICK]=true;assert(local_menu_host_recovery_frame(&s,1,&frame,0,240,640,240));
 assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_NEUTRAL);
 input_inventory_poll(&devices,device,identity,true,true,true,time_add_ms(now,900),owners,2);assert(status->armed);
 frame.connected=true;frame.pressed[IB_GUI_CLICK]=false;
 assert(local_menu_host_recovery_frame(&s,1,&frame,0,240,640,240));assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_READY);
	/* R-B: a direction and A in the same report cannot dismiss recovery. */
	frame.pressed[IB_GUI_DOWN]=frame.pressed[IB_GUI_CLICK]=true;
	assert(local_menu_host_recovery_frame(&s,1,&frame,0,240,640,240));
	assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_READY);
	frame.pressed[IB_GUI_DOWN]=frame.pressed[IB_GUI_CLICK]=false;
 /* A pointer press outside Resume must not dismiss; a deliberate hit does. */
 frame.pointer_valid=true;frame.pointer_x=10;frame.pointer_y=250;
 local_menu_host_recovery_frame(&s,1,&frame,0,240,640,240);frame.pointer_x=20;
 frame.pressed[IB_GUI_CLICK]=true;local_menu_host_recovery_frame(&s,1,&frame,0,240,640,240);
 assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_READY);
 /* A4 (10-31): Resume is the shared disconnect card's button; aim at the
  * centre of the rectangle its one layout returns (pane at y 240). */
 {struct controller_recovery_card card;struct recovery_card_layout l;
  assert(controller_recovery_card(&s.menu_host.recovery,frame.category,true,&card));
  recovery_card_layout(0,0,640,240,true,&card,true,frame.glyphs[IB_GUI_CLICK],&l);
  assert(l.has_button&&l.button.width>0);
  frame.pointer_x=l.button.x+l.button.width/2.0F;frame.pointer_y=240+l.button.y+l.button.height/2.0F;}
 assert(local_menu_host_recovery_frame(&s,1,&frame,0,240,640,240));assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_NONE);
 /* A spare channel already published connected is disarmed by its claim.
  * Even without an observed LOST frame it must enter deliberate recovery. */
 s.menu_host.recovery=(struct controller_recovery){0};
 devices.devices[device.channel].armed=false;frame.connected=false;
 assert(local_menu_host_recovery_frame(&s,1,&frame,0,480,640,240));
 assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_NEUTRAL);
 bound=false;
 puts("PASS physical recovery: published reconnect remains neutral-gated until armed; fresh Resume hit required");
}
static void test_attachment_transition_frame(void) {
 struct input_owner owners[2]={0};assert(input_owner_bind(owners,2,1,device));bound=true;
 struct input_identity identity={true,INPUT_CAT_CLASSIC_CONTROLLER};ptime_t now={0};
 input_inventory_poll(&devices,device,identity,true,true,true,now,owners,2);
 input_inventory_poll(&devices,device,identity,true,true,true,time_add_ms(now,250),owners,2);
 const struct input_device_status* status=input_inventory_device(&devices,device);
 assert(status->armed);
 struct client_session s={.local_ticket=8,.local_generation=9,.local_bound=true,
  .local_phase=CLIENT_GUEST_ATTACHED};
 assert(local_pause_open(&s.pause,false));s.pause.selected=0;
 s.pause.exit_pending=true;
 struct input_player_frame playable={.connected=true,.category=INPUT_CAT_CLASSIC_CONTROLLER};
 assert(!local_menu_host_recovery_frame(&s,1,&playable,0,240,640,240));
 s.pause.exit_pending=false; /* an authoritative Leave alone never suspends */
 /* The extension-loss report itself is hostile: it carries Down+Click and a
  * held gameplay action. Recovery must own it before Pause or gameplay can. */
 identity.category=INPUT_CAT_WIIMOTE;
 input_inventory_poll(&devices,device,identity,true,true,false,time_add_ms(now,300),owners,2);
 assert(!status->armed);
 struct input_player_frame frame={.connected=true,.category=INPUT_CAT_WIIMOTE};
 frame.down[IB_GUI_DOWN]=frame.down[IB_GUI_CLICK]=frame.down[IB_ACTION1]=true;
 frame.pressed[IB_GUI_DOWN]=frame.pressed[IB_GUI_CLICK]=frame.pressed[IB_ACTION1]=true;
 assert(local_menu_host_recovery_frame(&s,1,&frame,0,240,640,240));
 assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_LOST);
 assert(s.pause.selected==0&&!s.pause.exit_pending&&s.local_phase==CLIENT_GUEST_ATTACHED);
 /* A playable-family transition may re-arm from a genuinely neutral report;
  * the following press is then necessarily a fresh deliberate input. */
 identity.category=INPUT_CAT_NUNCHUK;
 input_inventory_poll(&devices,device,identity,true,true,true,time_add_ms(now,350),owners,2);
 assert(status->armed);
 frame=(struct input_player_frame){.connected=true,.category=INPUT_CAT_NUNCHUK};
 assert(local_menu_host_recovery_frame(&s,1,&frame,0,240,640,240));
 assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_READY);
 frame.pressed[IB_GUI_CLICK]=true;
 assert(local_menu_host_recovery_frame(&s,1,&frame,0,240,640,240));
 assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_NONE);
 bound=false;
 puts("PASS attachment transition: hostile loss frame suppressed; neutral then fresh Resume required");
}
static void test_collapsed_resume(void) {
 struct client_session s={.local_generation=99,.local_ticket=7};
 struct split_layout layout;split_layout_reset(&layout);
 bool lost[2]={false,false};
 split_layout_step(&layout,true,lost,0);split_layout_step(&layout,true,lost,.35F);
 assert(local_menu_host_recovery_frame(&s,1,NULL,0,480,640,240));
 assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_LOST);
 lost[1]=true;split_layout_step(&layout,true,lost,0);
 split_layout_step(&layout,true,lost,5);split_layout_step(&layout,true,lost,.35F);
 assert(layout.boundary==1 && s.local_generation==99 && s.local_ticket==7);
 struct input_player_frame f={.connected=true};
 assert(local_menu_host_recovery_frame(&s,1,&f,0,480,640,240));
 assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_READY);
 split_layout_step(&layout,true,lost,1);assert(layout.boundary==1);
 f.pressed[IB_GUI_CLICK]=true;
 assert(local_menu_host_recovery_frame(&s,1,&f,0,480,640,240));
 assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_NONE);
 lost[1]=false;split_layout_step(&layout,true,lost,0);
 split_layout_step(&layout,true,lost,.35F);assert(layout.boundary==.5F);
 assert(s.local_generation==99 && s.local_ticket==7);
 puts("PASS collapsed recovery: missing frame suspends; neutral plus fresh Resume restores the same session");
}
static void test_recovery_last_input_wins(void) {
	struct client_session s={0};
	s.menu_host.recovery.phase=CONTROLLER_RECOVERY_READY;
	struct input_player_frame f={.connected=true,.pointer_valid=true,
		.pointer_x=20,.pointer_y=250}; /* outside Resume */
	assert(local_menu_host_recovery_frame(&s,1,&f,0,240,640,240));
	f.pressed[IB_GUI_DOWN]=true;
	assert(local_menu_host_recovery_frame(&s,1,&f,0,240,640,240));
	f.pressed[IB_GUI_DOWN]=false;f.pressed[IB_GUI_CLICK]=true;
	assert(local_menu_host_recovery_frame(&s,1,&f,0,240,640,240));
	assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_NONE);
	/* A moved sprite on blank space owns the next A and cannot Resume. */
	s.menu_host.recovery.phase=CONTROLLER_RECOVERY_READY;
	s.menu_host.recovery_focus=(struct gutil_menu_focus){.initialized=true};
	f.pressed[IB_GUI_CLICK]=false;f.pointer_x=20;
	assert(local_menu_host_recovery_frame(&s,1,&f,0,240,640,240));
	f.pointer_x=40;f.pressed[IB_GUI_CLICK]=true;
	assert(local_menu_host_recovery_frame(&s,1,&f,0,240,640,240));
	assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_READY);
	puts("PASS recovery last input wins: ring Resume, moved blank pointer blocks");
}
/* Fable G4 review item 4: READY is entered with the focus seeded, so a sprite
 * that was already resting over blank when the controller came back is not
 * the focus owner; A resumes, as it did before G4. Once the sprite MOVES it
 * owns focus, and a blank press is inert (test_recovery_last_input_wins). */
static void test_recovery_unmoved_blank_sprite_resumes(void) {
	struct client_session s={0};
	s.menu_host.recovery.phase=CONTROLLER_RECOVERY_NEUTRAL;
	struct input_player_frame f={.connected=true,.pointer_valid=true,
		.pointer_x=20,.pointer_y=250}; /* outside Resume */
	assert(local_menu_host_recovery_frame(&s,1,&f,0,240,640,240));
	assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_READY);
	assert(local_menu_host_recovery_frame(&s,1,&f,0,240,640,240));
	assert(!s.menu_host.recovery_focus.pointer_owner
		&& "the first valid IR frame in READY made a resting sprite the owner");
	f.pressed[IB_GUI_CLICK]=true;
	local_menu_host_recovery_frame(&s,1,&f,0,240,640,240);
	assert(s.menu_host.recovery.phase==CONTROLLER_RECOVERY_NONE
		&& "A with an unmoved sprite over blank did not Resume");
	puts("PASS recovery unmoved blank sprite: READY entered seeded, A resumes");
}
/* critique-1028 glyph gaps: the disconnected-controller wording follows the
 * LOST family (GameCube is wired; every WPAD family uses 1+2); a split
 * pane's container prompts come from that pane's own frame; the walker's
 * glyph-proof family key. */
static void test_device_wording_and_owner_glyphs(void) {
 assert(!strcmp(local_menu_host_reconnect_hint(INPUT_CAT_GAMECUBE),"Plug the controller back in."));
 assert(!strcmp(local_menu_host_reconnect_hint(INPUT_CAT_NUNCHUK),"Press 1+2 on the Wii Remote."));
 assert(!strcmp(local_menu_host_reconnect_hint(INPUT_CAT_CLASSIC_CONTROLLER),"Press 1+2 on the Wii Remote."));
 assert(!strstr(local_menu_host_reconnect_hint(INPUT_CAT_GAMECUBE),"1+2"));
 struct input_player_frame p1={.connected=true,.category=INPUT_CAT_NUNCHUK},p2={.connected=true,.category=INPUT_CAT_GAMECUBE};
 for(int b=0;b<IB_COUNT;b++){p1.glyphs[b]=100+b;p2.glyphs[b]=200+b;}
 struct container_glyphs g1,g2;
 container_compact_player_glyphs(&g1,&p1);container_compact_player_glyphs(&g2,&p2);
 assert(g2.symbols[1]==200+IB_GUI_CLICK&&g1.symbols[1]==100+IB_GUI_CLICK);
 for(int k=0;k<CONTAINER_PROMPT_COUNT;k++)assert(g2.symbols[k]>=200&&g1.symbols[k]<200);
 assert(menu_autopilot_device_parse("gamecube")==INPUT_CAT_GAMECUBE);
 assert(menu_autopilot_device_parse("gc")==INPUT_CAT_GAMECUBE);
 assert(menu_autopilot_device_parse("classic")==INPUT_CAT_CLASSIC_CONTROLLER);
 assert(menu_autopilot_device_parse("nunchuk")==INPUT_CAT_NUNCHUK);
 assert(menu_autopilot_device_parse(NULL)==INPUT_CAT_NUNCHUK);
 assert(menu_autopilot_device_parse("bogus")==INPUT_CAT_NUNCHUK);
 puts("PASS device wording (GC wired vs 1+2), per-owner container glyphs, walker family key");
}

int main(void) {
 recover_mode(LOCAL_MENU_CONTAINER);recover_mode(LOCAL_MENU_RECIPES);recover_mode(LOCAL_MENU_MANUAL);assert(back_cues==3);
 test_physical_neutral();
 test_attachment_transition_frame();
 test_collapsed_resume();
	test_recovery_last_input_wins();
	test_recovery_unmoved_blank_sprite_resumes();
	test_device_wording_and_owner_glyphs();
 puts("PASS owned recovery: retained items, cursors, menu focus and generations; held reconnect inert; no gameplay/window sends");return 0;
}
