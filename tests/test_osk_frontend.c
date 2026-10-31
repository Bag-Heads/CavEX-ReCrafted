/* Frontend OSK: real mutator, geometry and canonical menu activation. No GL.
 * Link the canonical shared engine test pools; the text device queue is
 * wrapped for deterministic keyboard ownership and duplicate-input checks.
 * Legacy profiles have their independent unchanged test_osk_env/sign/chat guards. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "game/gui/widget_osk.h"
#include "graphics/gui_util.h"
#include "platform/input_owner.h"

static char queue[128];
static size_t head, tail;
static bool typing, mouse_click;
bool __wrap_input_text_active(void) { return typing; }
bool __wrap_input_text_mouse_pressed(enum input_button b) {
	return b == IB_GUI_CLICK && mouse_click;
}
bool __wrap_input_text_poll(char* out) {
	if(head == tail) return false;
	*out = queue[head++]; return true;
}
static void push(const char* s) {
	assert(strlen(s) < sizeof(queue));
	strcpy(queue, s); head = 0; tail = strlen(s);
}
static struct input_player_frame owner;
static enum osk_event step(struct osk_frontend* o, int button, bool ptr,
		float x, float y, bool keyboard, bool done) {
	memset(owner.down, 0, sizeof(owner.down));
	memset(owner.pressed, 0, sizeof(owner.pressed));
	if(button >= 0) owner.down[button] = owner.pressed[button] = true;
	return osk_frontend_update_owned(o,640,480,&owner,keyboard,ptr,x,y,true,done);
}
static void neutral(struct osk_frontend* o) {
	(void)step(o,-1,false,0,0,false,true);
}
static void reset(struct osk_frontend* o,char* b,size_t cap,int max) {
	owner=(struct input_player_frame){.connected=true,.category=INPUT_CAT_NUNCHUK};
	typing=mouse_click=false; head=tail=0;
	osk_frontend_reset(o,b,cap,max,0,false); neutral(o);
}
static void mutation(void) {
	char b[9]="abcd"; struct osk_frontend o;
	reset(&o,b,sizeof(b),6);
	o.caret=2;
	assert(osk_frontend_feed_char(&o,'X')==OSK_EV_CHANGED);
	assert(!strcmp(b,"abXcd")&&o.caret==3);
	assert(osk_frontend_feed_char(&o,'\b')==OSK_EV_CHANGED);
	assert(!strcmp(b,"abcd")&&o.caret==2);
	o.caret=0;
	assert(osk_frontend_feed_char(&o,'\b')==OSK_EV_NONE);
	assert(osk_frontend_feed_char(&o,'!')==OSK_EV_CHANGED);
	assert(!strcmp(b,"!abcd"));
	assert(osk_frontend_feed_char(&o,'?')==OSK_EV_CHANGED);
	assert(!strcmp(b,"!?abcd"));
	assert(osk_frontend_feed_char(&o,'z')==OSK_EV_NONE);
	assert(!strcmp(b,"!?abcd")&&o.caret==2);
	assert(osk_frontend_feed_char(&o,'\t')==OSK_EV_NEXT);
	assert(osk_frontend_feed_char(&o,'\r')==OSK_EV_DONE);
	assert(osk_frontend_feed_char(&o,27)==OSK_EV_CANCEL);
	assert(osk_frontend_feed_char(&o,1)==OSK_EV_NONE);
	assert(osk_frontend_feed_char(&o,(char)255)==OSK_EV_NONE);
	char tiny[3]="ab";
	osk_frontend_bind(&o,tiny,sizeof(tiny),0); o.caret=1;
	assert(osk_frontend_feed_char(&o,'c')==OSK_EV_NONE);
	assert(!strcmp(tiny,"ab")&&o.caret==1);
	assert(osk_frontend_feed_char(&o,'\b')==OSK_EV_CHANGED);
	assert(!strcmp(tiny,"b")&&o.caret==0);
	assert(osk_frontend_feed_char(&o,'c')==OSK_EV_CHANGED);
	assert(!strcmp(tiny,"cb"));
	/* The opt-in adaptation cannot alter legacy append/backspace semantics. */
	struct osk_state old; char plain[8]="abcd";
	osk_reset(&old,plain,sizeof(plain),0);
	assert(osk_feed_char(&old,'X')==OSK_EV_CHANGED&&!strcmp(plain,"abcdX"));
}
static void geometry_and_coverage(void) {
	struct osk_frontend o; char b[16]=""; reset(&o,b,sizeof(b),0);
	struct gutil_button_bounds r;
	assert(osk_frontend_key_bounds(&o,640,480,OSK_FRONT_CHAR_FIRST,&r));
	assert(r.x==32&&r.y==208&&r.width==54&&r.height==44);
	assert(osk_frontend_key_bounds(&o,802,480,OSK_FRONT_DONE,&r));
	/* Cancel / Done are 120 wide (10-31) so the hint row fits between. */
	assert(r.x==569&&r.y==408&&r.width==120&&r.height==44);
	assert(osk_frontend_key_bounds(&o,1280,960,OSK_FRONT_CANCEL,&r));
	assert(r.x==64&&r.y==816&&r.width==240&&r.height==88);
	assert(osk_frontend_key_bounds(&o,640,480,OSK_FRONT_LEFT,&r));
	assert(r.x==280&&r.y==356&&r.width==44&&r.height==44);
	bool reachable[127]={0}; reachable[' ']=true;
	for(int layout=0;layout<3;layout++) for(int upper=0;upper<2;upper++) {
		o.layout=layout; o.upper=upper;
		for(int i=0;i<30;i++) {
			char ch=osk_frontend_char(&o,i); if(!ch)continue;
			reachable[(unsigned char)ch]=true;
			assert(osk_frontend_key_bounds(&o,640,480,OSK_FRONT_CHAR_FIRST+i,&r));
			assert(r.x>=32&&r.x+r.width<=608&&r.y>=208&&r.y+r.height<=348);
			assert(gutil_button_hit(&r,r.x,r.y));
			assert(!gutil_button_hit(&r,r.x+r.width,r.y));
			assert(!gutil_button_hit(&r,r.x,r.y+r.height));
		}
	}
	for(int ch=32;ch<=126;ch++)assert(reachable[ch]);
	o.layout=1;o.address=true;
	assert(osk_frontend_char(&o,3)=='.'&&osk_frontend_char(&o,7)==':');
	assert(!osk_frontend_enabled(&o,OSK_FRONT_SPACE,true));
	assert(!osk_frontend_enabled(&o,OSK_FRONT_CASE,true));
	assert(osk_frontend_key_bounds(&o,640,480,OSK_FRONT_CHAR_FIRST,&r));
	assert(r.x==130&&r.y==208&&r.width==92&&r.height==44);
}
static void owner_and_events(void) {
	char b[32]=""; struct osk_frontend o; reset(&o,b,sizeof(b),31);
	assert(step(&o,IB_GUI_CLICK,false,0,0,false,true)==OSK_EV_CHANGED);
	assert(!strcmp(b,"q"));
	/* Repeated pressed bits while held cannot repeat an insertion. */
	assert(step(&o,IB_GUI_CLICK,false,0,0,false,true)==OSK_EV_NONE);
	assert(!strcmp(b,"q"));
	neutral(&o);
	owner.connected=false;
	assert(step(&o,IB_GUI_CLICK,false,0,0,false,true)==OSK_EV_NONE);
	owner.connected=true;
	assert(step(&o,IB_GUI_CLICK,false,0,0,false,true)==OSK_EV_NONE);
	neutral(&o);
	assert(step(&o,IB_GUI_CLICK,false,0,0,false,true)==OSK_EV_CHANGED);
	assert(!strcmp(b,"qq"));
	neutral(&o);
	assert(step(&o,IB_GUI_PAGE,false,0,0,false,true)==OSK_EV_NONE&&o.layout==1);
	neutral(&o); step(&o,IB_GUI_PAGE,false,0,0,false,true); assert(o.layout==2);
	neutral(&o); step(&o,IB_GUI_PAGE,false,0,0,false,true); assert(o.layout==2);
	neutral(&o); step(&o,IB_GUI_PREVIOUS,false,0,0,false,true); assert(o.layout==1);
	neutral(&o);
	owner.pressed[IB_GUI_PREVIOUS]=owner.down[IB_GUI_PREVIOUS]=true;
	owner.pressed[IB_GUI_QUICK_MOVE]=owner.down[IB_GUI_QUICK_MOVE]=true;
	assert(osk_frontend_update_owned(&o,640,480,&owner,false,false,0,0,true,true)==OSK_EV_NONE);
	assert(o.layout==0); /* Remapped Previous/Done collision must only page. */
	neutral(&o);
	owner.pressed[IB_GUI_PAGE]=owner.down[IB_GUI_PAGE]=true;
	owner.pressed[IB_GUI_QUICK_MOVE]=owner.down[IB_GUI_QUICK_MOVE]=true;
	assert(osk_frontend_update_owned(&o,640,480,&owner,false,false,0,0,true,true)==OSK_EV_NONE);
	assert(o.layout==1);
	neutral(&o);
	assert(step(&o,IB_GUI_QUICK_MOVE,false,0,0,false,false)==OSK_EV_NONE);
	neutral(&o); assert(step(&o,IB_GUI_QUICK_MOVE,false,0,0,false,true)==OSK_EV_DONE);
	neutral(&o); assert(step(&o,IB_INVENTORY,false,0,0,false,true)==OSK_EV_CANCEL);
	neutral(&o);
	/* Simultaneous navigation and activate consumes the activation. */
	owner.pressed[IB_GUI_DOWN]=owner.down[IB_GUI_DOWN]=true;
	owner.pressed[IB_GUI_CLICK]=owner.down[IB_GUI_CLICK]=true;
	assert(osk_frontend_update_owned(&o,640,480,&owner,false,false,0,0,true,true)==OSK_EV_NONE);
	assert(!strcmp(b,"qq"));
	/* The pointer cannot activate an unrelated default through empty space. */
	reset(&o,b,sizeof(b),31);
	assert(step(&o,IB_GUI_CLICK,true,630,470,false,true)==OSK_EV_NONE);
	/* Rendered key rectangles are the pointer rectangles, including scaling. */
	/* Release with IR still valid, then MOVE it. Reacquiring a lost pointer
	 * deliberately does not steal focus in the canonical arbitration helper. */
	step(&o,-1,true,630,470,false,true);
	step(&o,-1,true,90,220,false,true);
	assert(o.target==OSK_FRONT_CHAR_FIRST+1);
	assert(step(&o,IB_GUI_CLICK,true,90,220,false,true)==OSK_EV_CHANGED);
	assert(!strcmp(b,"qqw"));
	neutral(&o);
	o.target=OSK_FRONT_DONE; o.origin=GUTIL_MENU_ORIGIN_DIRECTIONAL;
	assert(step(&o,IB_GUI_CLICK,true,320,50,false,true)==OSK_EV_DONE);
	assert(!strcmp(b,"qqw"));
	o.origin=GUTIL_MENU_ORIGIN_POINTER;
	neutral(&o);
	/* A shared Activate/Close report consumes Activate and cannot also cancel. */
	owner.pressed[IB_GUI_CLICK]=owner.down[IB_GUI_CLICK]=true;
	owner.pressed[IB_INVENTORY]=owner.down[IB_INVENTORY]=true;
	assert(osk_frontend_update_owned(&o,640,480,&owner,false,true,630,470,true,true)==OSK_EV_NONE);
	assert(!strcmp(b,"qqw"));
	/* Case survives a layout round trip; caret utility acts at the insertion
	 * point rather than only changing a drawn cursor. */
	neutral(&o); o.target=OSK_FRONT_CASE;
	step(&o,IB_GUI_CLICK,false,0,0,false,true); assert(o.upper);
	neutral(&o); step(&o,IB_GUI_PAGE,false,0,0,false,true);
	neutral(&o); step(&o,IB_GUI_PREVIOUS,false,0,0,false,true);
	assert(o.layout==0&&osk_frontend_char(&o,0)=='Q');
	neutral(&o); o.target=OSK_FRONT_LEFT;
	step(&o,IB_GUI_CLICK,false,0,0,false,true); assert(o.caret==2);
	assert(osk_frontend_feed_char(&o,'X')==OSK_EV_CHANGED&&!strcmp(b,"qqXw"));
}
static void keyboard_and_reveal(void) {
	char b[32]="abcd"; struct osk_frontend o; reset(&o,b,sizeof(b),31);
	o.caret=2; push("X"); typing=true;
	assert(step(&o,IB_GUI_CLICK,false,0,0,true,true)==OSK_EV_CHANGED);
	assert(!strcmp(b,"abXcd")); /* queue + bound click must not type twice */
	neutral(&o); push("\rZ");
	assert(step(&o,-1,false,0,0,true,true)==OSK_EV_DONE);
	assert(head==1&&!strcmp(b,"abXcd"));
	push("Y");
	assert(step(&o,-1,false,0,0,false,true)==OSK_EV_NONE&&head==0);
	owner.connected=false;
	assert(step(&o,-1,false,0,0,true,true)==OSK_EV_NONE&&head==tail);
	o.caret=strlen(b);
	assert(osk_frontend_reveal(&o,16,0)==strlen(b));
	o.caret=0; assert(osk_frontend_reveal(&o,16,100)==0);
}
static void disconnected_keyboard_events(void) {
	char b[32]="base";
	struct osk_frontend o;
	reset(&o,b,sizeof(b),31);
	typing=true;
	assert(step(&o,-1,false,0,0,true,true)==OSK_EV_NONE);
	owner.connected=false;
	push("X\r\033");
	assert(step(&o,-1,false,0,0,true,true)==OSK_EV_NONE);
	assert(head==tail && !strcmp(b,"base"));
	/* Events arriving between the last disconnected poll and reconnect also
	 * belong to the old owner interval, including terminal actions. */
	push("\033\rZ");
	owner.connected=true;
	assert(step(&o,-1,false,0,0,true,true)==OSK_EV_NONE);
	assert(head==tail && !strcmp(b,"base"));
	push("Y");
	assert(step(&o,-1,false,0,0,true,true)==OSK_EV_CHANGED);
	assert(!strcmp(b,"baseY"));
}
static void directional_activation_with_background_pointer(void) {
	char b[32]="";
	struct osk_frontend o;
	reset(&o,b,sizeof(b),31);
	step(&o,-1,true,630,470,false,true);
	step(&o,IB_GUI_RIGHT,true,630,470,false,true);
	step(&o,-1,true,630,470,false,true);
	assert(o.target==OSK_FRONT_CHAR_FIRST+1);
	assert(step(&o,IB_GUI_CLICK,true,630,470,false,true)==OSK_EV_CHANGED);
	assert(!strcmp(b,"w"));
}
static void unbound_pc_keyboard(void) {
	char b[32]="";
	struct osk_frontend o;
	struct input_player_frame scratch;
	/* Start as the real PC caller does, without first admitting a Nunchuk
	 * frame (the generic fixture reset would create an owner transition). */
	typing=mouse_click=false; head=tail=0;
	osk_frontend_reset(&o,b,sizeof(b),31,0,false);
	assert(!input_player_frame(0));
	const struct input_player_frame* frame=osk_frontend_input_frame(&scratch);
	assert(frame==&scratch && frame->connected);
	push("X"); typing=true;
	assert(osk_frontend_update_owned(&o,640,480,frame,true,false,0,0,
		true,true)==OSK_EV_CHANGED);
	assert(!strcmp(b,"X"));
	/* An assigned disconnected keyboard must never inherit the fallback. */
	assert(input_player_bind(0,(struct input_device){INPUT_DEVICE_KEYBOARD,0}));
	frame=osk_frontend_input_frame(&scratch);
	assert(frame==input_player_frame(0) && frame!=&scratch);
	assert(!frame->connected);
	push("Y");
	assert(osk_frontend_update_owned(&o,640,480,frame,true,false,0,0,
		true,true)==OSK_EV_NONE && head==tail);
	input_player_unbind(0);
}
int main(void) {
	mutation(); geometry_and_coverage(); owner_and_events(); disconnected_keyboard_events(); keyboard_and_reveal();
	directional_activation_with_background_pointer();
	unbound_pc_keyboard();
	puts("frontend OSK: insertion/caps/events, real geometry/ASCII coverage, owner/fresh-edge isolation and reveal PASS");
	return 0;
}
