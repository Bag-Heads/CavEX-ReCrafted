/* Complete container draw-list fixtures, including real footer/glyph rendering.
 * Only the device/graphics boundary is replaced; no widget/position filtering. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game/game_state.h"
#include "graphics/gui_util.h"
#include "game/gui/screen.h"
#include "block/blocks.h"
#include "game/gui/container_menu.h"
#include "game/gui/container_compact.h"
#include "platform/menu_sound.h"
#include "platform/audio.h"
#include "platform/input_glyph.h"
#include "platform/texture.h"
#include "platform/gfx.h"
#include "item/items.h"
#include "item/inventory_sort.h"
static char calls[131072];
static size_t used;
static bool recording,texture_enabled=true;
static unsigned viewport_shades;
static struct tex_gfx* bound_texture;
static unsigned fire_idle,arrow_idle,flame_live,arrow_live;
static bool region(size_t n,const int16_t* v,const uint16_t* uv,int sx,int sy,int sw,int sh,int dw,int dh){
 if(n!=4)return false;
 int x0=v[0],x1=v[0],y0=v[1],y1=v[1],u0=uv[0],u1=uv[0],v0=uv[1],v1=uv[1];
 for(size_t k=1;k<4;k++){
  if(v[k*3]<x0)x0=v[k*3];
  if(v[k*3]>x1)x1=v[k*3];
  if(v[k*3+1]<y0)y0=v[k*3+1];
  if(v[k*3+1]>y1)y1=v[k*3+1];
  if(uv[k*2]<u0)u0=uv[k*2];
  if(uv[k*2]>u1)u1=uv[k*2];
  if(uv[k*2+1]<v0)v0=uv[k*2+1];
  if(uv[k*2+1]>v1)v1=uv[k*2+1];
 }
 return u0==sx&&v0==sy&&u1-u0==sw&&v1-v0==sh&&x1-x0==dw&&y1-y0==dh;
}
static unsigned sounds[16], sound_count;
static void record(const char* fmt, ...) {
    if(!recording) return;
    va_list ap; va_start(ap,fmt);
    int n=vsnprintf(calls+used,sizeof(calls)-used,fmt,ap); va_end(ap);
    assert(n>=0 && (size_t)n<sizeof(calls)-used); used+=(size_t)n;
}
void __wrap_gfx_bind_texture(struct tex_gfx* t) {
    bound_texture=t;
    const char* name=t==&texture_gui_crafting?"crafting":t==&texture_gui_furnace?"furnace":t==&texture_gui_chest?"chest":t==&texture_gui_widgets?"widgets":
        t==&texture_icons?"icons":t==&texture_controls?"controls":t==&texture_font?"font":t==&texture_pointer?"pointer":
        t==&texture_mob_char?"player":NULL; /* T13: the one split page draws the portrait */
    assert(name); record("bind %s\n",name);
}
void __wrap_gfx_texture(bool enabled) { texture_enabled=enabled;record("texture %d\n",enabled); }
/* T13: the one-page split inventory always draws the player portrait; this
 * GL-less harness keeps its 3D state calls inert (the recorded draw list --
 * gfx_draw_quads / gfx_bind_texture -- is unchanged). */
void __wrap_gfx_matrix_modelview(mat4 mv) { (void)mv; }
void __wrap_gfx_write_buffers(bool color,bool depth,bool depth_test) { (void)color;(void)depth;(void)depth_test; }
void __wrap_gfx_lighting(bool enable) { (void)enable; }
void __wrap_gfx_cull_func(enum cull_func func) { (void)func; }
void __wrap_gfx_draw_quads(size_t n,const int16_t* v,const uint8_t* c,const uint16_t* uv) {
    if(!texture_enabled&&n==4&&v[0]==0&&v[1]==0&&v[3]==640&&v[7]==240&&c[3]==180)viewport_shades++;
    if(bound_texture==&texture_gui_furnace){
     if(region(n,v,uv,56,36,14,14,20,20))fire_idle++;
     if(region(n,v,uv,79,34,22,15,44,30))arrow_idle++;
     if(region(n,v,uv,176,6,14,8,20,11))flame_live++;
     if(region(n,v,uv,176,14,13,16,22,30))arrow_live++;
    }
    record("quads %zu",n);
    for(size_t i=0;i<n;i++) record(" | %d,%d,%d %u,%u,%u,%u %u,%u",
        v[i*3],v[i*3+1],v[i*3+2],c[i*4],c[i*4+1],c[i*4+2],c[i*4+3],uv[i*2],uv[i*2+1]);
    record("\n");
}
void __wrap_gutil_draw_item(struct item_data* item,int x,int y,int layer) {
    record("item %u %u %u %d %d %d\n",item->id,item->count,item->durability,x,y,layer);
}
int __wrap_gfx_width(void) { return 640; }
int __wrap_gfx_height(void) { return 480; }
void __wrap_input_pointer_enable(bool enable) {}
/* Exact Beta bitmap advances embedded by the frozen prototype; normal font
 * measurement and rendering both run against this texture-read fixture. */
static const int font_widths[256] = {5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,4,1,4,5,5,5,5,2,4,4,4,5,1,5,1,5,5,5,5,5,5,5,5,5,5,5,1,1,4,5,4,5,6,5,5,5,5,5,5,5,5,3,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,3,5,3,5,5,2,5,5,5,5,5,4,5,5,1,5,4,2,5,5,5,5,5,5,5,3,5,5,5,5,5,5,4,1,4,6,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5};
void __wrap_tex_gfx_lookup(struct tex_gfx* t,int x,int y,uint8_t* rgba) {
    assert(t==&texture_font);
    int k=x/8+(y/8)*16;
    memset(rgba,x%8<font_widths[k]?255:0,4);
}
void __wrap_audio_menu_event(enum menu_sound_event e,ptime_t now) {
    assert(sound_count<16); sounds[sound_count++]=e;
}
static bool send_ok(void* user,struct server_rpc* rpc) { return true; }
static bool send_fail(void* user,struct server_rpc* rpc) { return false; }
/* Independent frozen CSS geometry oracle: chest.html:10-15,23,135,146.
 * Each device's real glyph is drawn at the declared 24px header size. */
static void check_header_rects(const struct input_player_frame* frame) {
    const char* titles[] = {"Chest", "Large chest", "Chest", "Chest 1", "Chest 2", "Inventory"};
    const int widths[] = {56, 126, 56, 78, 78, 100};
    for(int k=0;k<6;k++) for(int offset=0;offset<=240;offset+=240) {
        int x=k<2?146:32, y=(k==0?24:k==1?12:8)+offset, w=k<2?348:576;
        struct gutil_button_bounds r[4];
        container_chest_header_rects(x,y,w,titles[k],r);
        assert(r[0].x==x+12 && r[0].y==y+8 && r[0].width==24);
        assert(r[1].x==x+40 && r[1].width==widths[k]);
        assert(r[2].x==x+44+widths[k] && r[2].width==24);
        assert(r[3].x==x+w-68 && r[3].width==56);
        for(int a=0;a<4;a++) {
            assert(r[a].y==y+8 && r[a].height==24);
            assert(r[a].x>=x+12 && r[a].x+r[a].width<=x+w-12);
            for(int b=0;b<a;b++)
                assert(r[a].x>=r[b].x+r[b].width || r[b].x>=r[a].x+r[a].width);
        }
        assert(gutil_prompt_glyph(r[0].x,r[0].y,24,frame->glyphs[IB_GUI_PREVIOUS])==r[0].width);
        assert(gutil_prompt_glyph(r[2].x,r[2].y,24,frame->glyphs[IB_GUI_PAGE])==r[2].width);
    }
}
static void check_draw(struct window_container* wc,int mode,int controller,int state) {
    struct container_compact compact;
    int height=mode==2?240:480;
    if(mode==2) assert(container_compact_open(&compact,WINDOW_TYPE_CHEST,wc->slot_count,0,640,height));
    else {
        screen_chest_set_windowc(WINDOWC_CHEST);
        screen_chest.reset(&screen_chest,640,height);
        /* Real navigation reaches header and opens menu; no screen-state wrapper. */
        config_set_string(&gstate.config_user,"debug.container_glyphs", "auto");
    }
    struct input_player_frame frame;
    const char* devices[]={"wiimote","classic","gamecube"};
    config_set_string(&gstate.config_user,"debug.container_glyphs",devices[controller]);
    input_prompt_frame(0,&frame);
    const int glyphs[]={0,10,32}; assert(frame.glyphs[IB_GUI_CLICK]==glyphs[controller]);
    check_header_rects(&frame);
    /* Sort parked 2026-10-25 (maintainer): stale Sort state from any path is
     * cleared by the first real step, so nothing below can draw Sort. */
    if(mode==2) {
        compact.menu.sort_focus=true; compact.menu.sort_open=state!=0; compact.menu.sort_operation=0;
        struct container_input idle={0};
        container_compact_step(&compact,wc,WINDOWC_CHEST,&idle,.016F,send_ok,NULL);
        assert(!compact.menu.sort_focus && !compact.menu.sort_open);
    }
    /* Full-screen update reads the same real frame path. Feed action levels at
     * the legacy input hardware boundary, then render screen_chest itself. */
    extern bool fixture_up, fixture_pick;
    if(mode!=2) {
        /* Reset selection starts in player main slots; previous section -> chest. */
        extern bool fixture_previous;
        fixture_previous=true;
        screen_chest.update(&screen_chest,.016F);screen_chest.update(&screen_chest,.016F);
        fixture_previous=false;
        fixture_up=true; screen_chest.update(&screen_chest,.016F); fixture_up=false;
        screen_chest.update(&screen_chest,.016F);
        /* UP off the top row used to focus the Sort entry; now it is inert. */
        assert(!screen_chest_sort_open());
    }
    wc->sort.status=WINDOW_SORT_IDLE;
    windowc_get_latest(wc)->picked_item=(struct item_data){0};
    if(state==2) windowc_get_latest(wc)->picked_item=(struct item_data){.id=1,.count=1};
    recording=true;used=0;calls[0]=0;
    if(mode==2) {
        struct container_glyphs glyph;
        container_compact_active_glyphs(&glyph);
        container_compact_render(&compact,wc,0,0,&glyph);
    } else screen_chest.render2D(&screen_chest,640,height);
    recording=false;
    char path[160];snprintf(path,sizeof(path),"tests/golden/sort_container/%s-%s-%s.txt",
        mode==0?"single":mode==1?"large":"compact",devices[controller],
        state==0?"header":state==1?"menu":"blocked");
    if(getenv("CW_UPDATE_SORT_GOLDENS")) {
        FILE* f=fopen(path,"wb");assert(f);assert(fwrite(calls,1,used,f)==used);fclose(f);
    } else {
        char expected[sizeof(calls)];FILE* f=fopen(path,"rb");assert(f);
        size_t n=fread(expected,1,sizeof(expected),f);assert(!ferror(f));fclose(f);
        if(n!=used || memcmp(calls,expected,n)) {fprintf(stderr,"draw-list mismatch: %s\n",path);assert(0);}
    }
    /* No Sort panel can cover the container: slots/items/footer always draw. */
    assert(strstr(calls,"item ")!=NULL);
    /* Q7: the large chest is the single chest's 3-row panel, paged. */
    assert((strstr(calls,"bind icons\n")!=NULL)==(mode!=2));
    assert((strstr(calls,"item 4 7 0 ")!=NULL)==(mode!=2));
    /* No Sort control is drawn or focusable: the former "open Sort" input
     * sequence (state 1) leaves a draw list identical to the plain header. */
    static char header_calls[sizeof(calls)]; static size_t header_used;
    if(state==0) { memcpy(header_calls,calls,used); header_used=used; }
    if(state==1) assert(used==header_used && !memcmp(calls,header_calls,used));
    windowc_get_latest(wc)->picked_item=(struct item_data){0};
    sound_count=0;
}
static void check_machine_draw(void){
 struct window_container machine;
 struct window_container* saved=gstate.players[0].windows[WINDOWC_CHEST];
 struct container_glyphs glyph;for(int k=0;k<CONTAINER_PROMPT_COUNT;k++)glyph.symbols[k]=-1;
 for(int type=0;type<2;type++){
  enum window_type kind=type?WINDOW_TYPE_DISPENSER:WINDOW_TYPE_FURNACE;
  size_t count=type?DISPENSER_SIZE:FURNACE_SIZE;
  assert(windowc_create(&machine,kind,count));gstate.players[0].windows[WINDOWC_CHEST]=&machine;
  for(int h=480;h>=240;h-=240){
   if(type){screen_dispenser_set_windowc(WINDOWC_CHEST);screen_dispenser.reset(&screen_dispenser,640,h);}
   else{screen_furnace_set_windowc(WINDOWC_CHEST);screen_furnace.reset(&screen_furnace,640,h);}
   struct container_compact* v=&gstate.players[0].container;
   assert(v->active&&v->count==count&&v->view_count==1&&v->menu.slot_extent==36);
  }
  if(!type){
   struct container_compact* v=&gstate.players[0].container;v->menu.selected=0;
   fire_idle=arrow_idle=flame_live=arrow_live=0;
   recording=true;used=0;calls[0]=0;container_compact_render(v,&machine,0,0,&glyph);recording=false;
   assert(fire_idle==1&&arrow_idle==1&&!flame_live&&!arrow_live);
   machine.furnace_current_item_burn=200;machine.furnace_burn_time=100;machine.furnace_cook_time=100;
   recording=true;used=0;calls[0]=0;container_compact_render(v,&machine,0,0,&glyph);recording=false;
   assert(flame_live==1&&arrow_live==1);
   struct inventory* inv=windowc_get_latest(&machine);inv->items[v->slots[0].slot]=(struct item_data){.id=1,.count=3};
   inv->picked_item=(struct item_data){.id=4,.count=7};v->menu.pointer_available=v->menu.pointer_on_slot=true;
   v->menu.pointer_x=400;v->menu.pointer_y=100;
   recording=true;used=0;calls[0]=0;container_compact_render(v,&machine,0,0,&glyph);recording=false;
   assert(strstr(calls,"item 1 3 0 76 57 1\n")&&strstr(calls,"item 4 7 0 384 84 0\n"));
  }
  windowc_destroy(&machine);
 }
 gstate.players[0].windows[WINDOWC_CHEST]=saved;
}
bool fixture_up, fixture_pick, fixture_previous;
bool __wrap_input_held(enum input_button b) { return b==IB_GUI_UP && fixture_up; }
bool __wrap_input_pressed(enum input_button b) {
    return (b==IB_GUI_UP && fixture_up)||(b==IB_GUI_CLICK && fixture_pick)||(b==IB_GUI_PREVIOUS && fixture_previous);
}
bool __wrap_input_pointer(float* x,float* y,float* a) { *x=*y=*a=0;return false; }
int main(void) {
	/* Draw recorder: represent a loaded fallback sheet; graphics calls are wrapped. */
	static uint8_t controls_texel[4];
	texture_controls.data = controls_texel;
    gstate.config_user.root=json_value_init_object();
    texture_font.width=texture_font.height=128;gutil_reset_font(&texture_font);
    assert(gutil_font_width("Sort",16)==44);
    audio_init(false,100);
    const uint16_t sample_ids[]={SOUND_MENU_SCROLL,SOUND_MENU_SECTION,SOUND_MENU_SELECT};
    for(int i=0;i<3;i++) {
        const struct audio_sample* sample=audio_registry_pick(sample_ids[i]);
        assert(sample && sample->pcm && sample->frames && sample->rate==48000);
    }
    struct window_container wc;
    struct window_container player;
    assert(windowc_create(&player,WINDOW_TYPE_INVENTORY,45));
    gstate.players[0].windows[WINDOWC_INVENTORY]=&player;
    struct entity hud_body={0};hud_body.health=160;
    gstate.players[0].local_player=&hud_body;
    windowc_get_latest(&player)->items[INVENTORY_SLOT_HOTBAR]=(struct item_data){.id=4,.count=7};
    for(int m=0;m<3;m++) {
        assert(windowc_create(&wc,WINDOW_TYPE_CHEST,m==1?90:63));
        gstate.players[0].windows[WINDOWC_CHEST]=&wc;
        windowc_get_latest(&wc)->items[0]=(struct item_data){.id=1,.count=32};
        windowc_get_latest(&wc)->items[wc.slot_count-36]=(struct item_data){.id=4,.count=12};
        for(int c=0;c<3;c++) for(int state=0;state<3;state++) check_draw(&wc,m,c,state);
        windowc_destroy(&wc);
    }
    assert(windowc_create(&wc,WINDOW_TYPE_CHEST,63));
    gstate.players[0].windows[WINDOWC_CHEST]=&wc;
    wc.sort.status=WINDOW_SORT_IDLE;
    struct container_menu menu={0};
    struct container_slot slots[]={{0,0,0,0},{36,0,1,0},{72,0,2,0}};
    struct container_input input={.right=true};
    container_menu_step(&menu,slots,3,0,0,&wc,WINDOWC_CHEST,&input,.016F,send_ok,NULL);
    assert(sound_count==1 && sounds[0]==MENU_SOUND_FOCUS);
    input=(struct container_input){0};
    container_menu_step(&menu,slots,3,0,0,&wc,WINDOWC_CHEST,&input,.016F,send_ok,NULL);
    /* Successful pick, then Back after a no-op empty pick/failure. The pick is
     * COMMIT-timed now (sound-events.md: "Actual transfer/save success hooks
     * must come from authoritative completed operations"), so the submit
     * itself is silent and the approved ITEM cue lands on the accept. */
    blocks_init();items_init();
    check_machine_draw();
    char wear[128];
    windowc_get_latest(&wc)->items[0]=(struct item_data){.id=274,.count=1,.durability=17};
    container_menu_label(&menu,&wc,0,wear,sizeof wear);
    assert(!strcmp(wear,"Stone Pickaxe - Wear 17 / 131"));
    windowc_get_latest(&wc)->picked_item=windowc_get_latest(&wc)->items[0];
    container_menu_label(&menu,&wc,0,wear,sizeof wear);
    assert(!strcmp(wear,"Holding: Stone Pickaxe - Wear 17 / 131"));
    windowc_get_latest(&wc)->picked_item=(struct item_data){0};
    windowc_get_latest(&wc)->items[0]=(struct item_data){.id=35,.count=1,.durability=3};
    container_menu_label(&menu,&wc,0,wear,sizeof wear);
    assert(!strstr(wear,"Wear"));
    windowc_get_latest(&wc)->items[1]=(struct item_data){.id=1,.count=1};
    input.pick=true;
    container_menu_step(&menu,slots,3,0,0,&wc,WINDOWC_CHEST,&input,.016F,send_ok,NULL);
    assert(sound_count==1);
    windowc_action_apply_result(&wc,windowc_get_latest(&wc)->revision.action_id,true);
    input=(struct container_input){0};
    container_menu_step(&menu,slots,3,0,0,&wc,WINDOWC_CHEST,&input,.016F,send_ok,NULL);
    assert(sound_count==2 && sounds[1]==MENU_SOUND_ITEM);
    windowc_discard_pending_actions(&wc);
    inventory_clear(windowc_get_latest(&wc));
    input=(struct container_input){.close=true};
    assert(container_menu_step(&menu,slots,3,0,0,&wc,WINDOWC_CHEST,&input,.016F,send_ok,NULL));
    assert(sound_count==3 && sounds[2]==MENU_SOUND_BACK);
    input=(struct container_input){0};
    wc.sort.status=WINDOW_SORT_COMPLETE;
    container_menu_step(&menu,slots,3,0,0,&wc,WINDOWC_CHEST,&input,.016F,send_ok,NULL);
    wc.sort.status=WINDOW_SORT_INTERRUPTED;
    container_menu_step(&menu,slots,3,0,0,&wc,WINDOWC_CHEST,&input,.016F,send_ok,NULL);
    assert(sound_count==3);
    wc.sort.status=WINDOW_SORT_IDLE;input.pick=true;
    container_menu_step(&menu,slots,3,0,0,&wc,WINDOWC_CHEST,&input,.016F,send_fail,NULL);
    assert(sound_count==3);
    /* Exercise actual reconciliation and rejected-ACK transitions, not just
     * rendering terminal status values. The accepted predictions are the
     * same no-echo success path used by vanilla. */
    struct entity body={0};
    struct client_session session={.local_player=&body,.visible_window=WINDOWC_CHEST};
    session.windows[WINDOWC_CHEST]=&wc;
    for(int reject=0;reject<2;reject++) {
        windowc_discard_pending_actions(&wc);
        inventory_clear(windowc_get_latest(&wc));
        windowc_get_latest(&wc)->items[1]=(struct item_data){.id=1,.count=1};
        wc.sort.status=WINDOW_SORT_IDLE;
        assert(client_session_sort_request(&session,WINDOWC_CHEST,
            INVENTORY_SORT_CHEST_GROUP,true,time_get(),send_ok,NULL));
        for(int tick=0;tick<20 && wc.sort.status==WINDOW_SORT_RUNNING;tick++) {
            client_session_sort_tick(&session,WINDOWC_CHEST,time_get(),send_ok,NULL);
            while(wc.sort.status==WINDOW_SORT_RUNNING && wc.sort.acknowledged<wc.sort.submitted)
                windowc_action_apply_result(&wc,wc.sort.actions[wc.sort.acknowledged%WINDOW_SORT_PIPELINE],!reject);
        }
        assert(wc.sort.status==(reject?WINDOW_SORT_INTERRUPTED:WINDOW_SORT_COMPLETE));
        /* A COMMITTED sort now confirms once (crafting-chests.md:27,
         * "completed successful craft/sort"); the rejected run stays silent.
         * Timing/uniqueness are pinned by test_sort_completion_sound. */
        assert(sound_count==4 && sounds[3]==MENU_SOUND_SELECT);
    }
    struct container_compact compact;
    assert(container_compact_open(&compact,WINDOW_TYPE_INVENTORY,45,0,640,240));
    struct container_input page={.page=true};
    container_compact_step(&compact,&player,0,&page,.016F,send_ok,NULL);
    /* ui-build-1026/T13: the split inventory is ONE page; Next is the same
     * section jump as full (hotbar -> armor/craft), never a view change. */
    assert(compact.view==0&&compact.view_count==1
        &&compact.slots[compact.menu.selected].slot<INVENTORY_SLOT_MAIN);
    struct container_glyphs glyphs;container_compact_active_glyphs(&glyphs);
    inventory_clear(windowc_get_latest(&player));
    viewport_shades=0;container_compact_render(&compact,&player,0,0,&glyphs);
    assert(viewport_shades==1);
    windowc_destroy(&wc);windowc_destroy(&player);audio_shutdown();config_destroy(&gstate.config_user);
    puts("PASS 27 complete container draw-list goldens; focus/item/back; committed sort confirms, rejection silent");
}
