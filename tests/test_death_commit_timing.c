/* Lane 5 -- the APPROVED death screen: commit-timed confirmation, the
 * preserved timing gates and the affected-viewport composition.
 *
 * Authority: docs/ui_design/approved/specs/menus.md ("Death covers the full
 * single-player screen or affected split viewport, with the existing gradient
 * and the revised layout. Preserve 400 ms entry protection, 750 ms Respawn
 * hold and 1,500 ms exit hold from the approved prototype; completed native
 * operations decide success.") and specs/sound-events.md, "Death screen"
 * ("No new death stinger. Existing scroll on changed selection. Hold start,
 * progress and cancelled hold are silent. Confirmation only when respawn
 * actually commits. ... Never indicate save success early.").
 *
 * The REAL screen_death.c runs; only the device/graphics/transport boundary is
 * doubled. The regression this exists to prevent is the pre-approval
 * behaviour: a quarter-volume click at ACTIVATION, i.e. a success sound before
 * the server had done anything -- the "false-success audio" the runtime rules
 * forbid. audio_play_ui below therefore aborts if it is reached at all. */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "game/autopilot.h"
#include "game/game_state.h"
#include "game/gui/menu_header.h"
#include "game/gui/screen.h"
#include "graphics/gui_util.h"
#include "network/server_interface.h"
#include "platform/menu_sound.h"
#include "platform/input_glyph.h"
#include "platform/input_owner.h"
#include "game/gui/screen_death.h"

/* ---- boundary doubles --------------------------------------------------- */

struct game_state gstate;
struct screen screen_ingame, screen_load_world, screen_title;
struct tex_gfx texture_pointer, texture_font, texture_controls,
	texture_gui_widgets;

static unsigned respawn_rpcs, unload_rpcs;
static unsigned owned_respawn_rpcs, leave_requests, leave_flushes;
static struct server_rpc_source owned_source;
static struct server_rpc owned_rpc;
static bool reject_rpc;
static enum menu_sound_event cues[32];
static size_t cue_count;
static struct screen* current;
static bool pressed[IB_COUNT], held[IB_COUNT];
static bool pointer_on;
static float ptr_x, ptr_y;
static int view_w = 640, view_h = 480;
static struct entity local_player;

bool svin_rpc_send(struct server_rpc* rpc) {
	if(reject_rpc)
		return false;
	if(rpc->type == SRPC_RESPAWN)
		respawn_rpcs++;
	else if(rpc->type == SRPC_UNLOAD_WORLD)
		unload_rpcs++;
	else
		assert(0 && "the death screen sends only Beta's two RPCs");
	return true;
}
bool svin_rpc_send_from(struct server_rpc_source source,struct server_rpc* rpc) {
	if(reject_rpc)return false;
	owned_source=source;owned_rpc=*rpc;owned_respawn_rpcs++;return true;
}
bool client_session_try_local_leave(struct client_session* s) {
	leave_requests++;
	if(reject_rpc)return false;
	s->local_phase=CLIENT_GUEST_LEAVING;s->local_death_leave_manual=true;
	return true;
}
void client_session_flush_local(struct client_session* s) {(void)s;leave_flushes++;}
void audio_menu_event(enum menu_sound_event e, ptime_t now) {
	(void)now;
	assert(cue_count < 32);
	cues[cue_count++] = e;
}
void audio_play_ui(uint16_t sound, float volume, float pitch) {
	(void)sound;
	(void)volume;
	(void)pitch;
	assert(0 && "the approved death screen plays no direct UI sound");
}
void screen_set(struct screen* s) {
	current = s;
	if(s->reset)
		s->reset(s, view_w, view_h);
}
void screen_notice_compose(int x, int y, int w, int h, bool in_header) {
 (void)in_header;
	(void)x;
	(void)y;
	(void)w;
	(void)h;
}
int gfx_width(void) {
	return view_w;
}
int gfx_height(void) {
	return view_h;
}
void gfx_texture(bool enabled) {
	(void)enabled;
}
void gfx_bind_texture(struct tex_gfx* t) {
	(void)t;
}
void gfx_draw_quads(size_t count, const int16_t* v, const uint8_t* color,
					const uint16_t* uv);
static int16_t gradient_v[12];
static uint8_t gradient_c[16];
static unsigned gradient_quads,glyph_calls;
static unsigned prompt_owner;
static bool yellow_score;
/* critique-1028 death redesign: the footer reads "Move" / "Hold to
 * choose"; the old in-button "Hold" and footer "Select" are gone. */
static bool saw_move,saw_hold_choose,saw_old_label,saw_respawn_label;
void gfx_draw_quads(size_t count, const int16_t* v, const uint8_t* color,
					const uint16_t* uv) {
	(void)uv;
	assert(count == 4);
 if(color[0]!=0x50||color[1]!=0||color[3]!=0x60)return;
	memcpy(gradient_v, v, sizeof(gradient_v));
	memcpy(gradient_c, color, sizeof(gradient_c));
	gradient_quads++;
}
/* The autopilot half of the input layer, wired exactly as source/platform/
 * input.c wires it (this test cannot link input.c -- it needs GLFW/WPAD -- so
 * the shared rising-edge algorithm comes from autopilot.h, the production
 * header, and only the "which button is forced" source is doubled). Inert
 * unless gstate.autopilot.active: every other case below leaves it false, so
 * their behaviour is unchanged. */
static bool forced_prev[IB_COUNT]; /* = input.c's input_autopilot_forced_prev */
static unsigned click_edges;       /* press edges screen_death actually saw */

/* autopilot.c's death-click script: released at tick 1 on screen_death (so the
 * fresh-press latch can arm) and for one tick every AUTOPILOT_DEATH_CLICK_-
 * RETRY_TICKS after it, held in between. Only "which button is forced" is
 * doubled -- the LEVEL comes from autopilot.h's shared
 * autopilot_death_click_held, the same formula autopilot.c calls. */
static bool force_button(enum input_button b, bool* out) {
	if(b == IB_GUI_CLICK
	   && gstate.players[0].current_screen == &screen_death) {
		*out = autopilot_death_click_held(
			gstate.autopilot.death_screen_ticks);
		return true;
	}
	return false;
}

bool input_pressed(enum input_button b) {
	if(gstate.autopilot.active) {
		bool forced;
		if(force_button(b, &forced)) {
			bool edge = autopilot_force_edge_pressed(forced_prev, b, forced);
			if(edge && b == IB_GUI_CLICK)
				click_edges++;
			return edge;
		}
	}
	return b < IB_COUNT && pressed[b];
}
bool input_held(enum input_button b) {
	if(gstate.autopilot.active) {
		bool forced;
		if(force_button(b, &forced))
			return forced;
	}
	return b < IB_COUNT && held[b];
}
void input_pointer_enable(bool enable) {
	(void)enable;
}
bool input_pointer(float* x, float* y, float* angle) {
	if(!pointer_on)
		return false;
	*x = ptr_x;
	*y = ptr_y;
	*angle = 0.0f;
	return true;
}
int gutil_font_width(const char* str, int scale) {
	return (int)strlen(str) * scale;
}
void gutil_text(int x, int y, const char* s, int scale, bool shadow) {
	(void)x;
	(void)y;
	(void)scale;
	(void)shadow;
	if(!strcmp(s,"Move"))saw_move=true;
	if(!strcmp(s,"Hold to choose"))saw_hold_choose=true;
	if(!strcmp(s,"Hold")||!strcmp(s,"Select"))saw_old_label=true;
}
void gutil_texquad_rt_any(int x, int y, float angle, int tx, int ty, int sx,
						  int sy, float w, float h) {
	(void)x; (void)y; (void)angle; (void)tx; (void)ty; (void)sx; (void)sy;
	(void)w; (void)h;
}
static char drawn_labels[4][32];
static int drawn_count;
void gutil_button_draw_dwell(int x, int y, int w_units, const char* label,
							 bool enabled, bool hover, float fill) {
	(void)x; (void)y; (void)w_units; (void)enabled; (void)hover; (void)fill;
	assert(drawn_count < 4);
	snprintf(drawn_labels[drawn_count++], 32, "%s", label);
}
int gutil_control_icon(int x, enum input_button b, const char* str) {
	(void)x;
	(void)b;
	(void)str;
	return 32;
}
bool gutil_button_hit(const struct gutil_button_bounds* b, float x, float y) {
	return x >= b->x && x < b->x + b->width && y >= b->y
		&& y < b->y + b->height;
}

/* ---- helpers ------------------------------------------------------------ */

static void enter_death(void) {
	memset(pressed, 0, sizeof(pressed));
	memset(held, 0, sizeof(held));
	pointer_on = false;
	cue_count = 0;
	drawn_count = 0;
	gradient_quads = glyph_calls = 0;yellow_score=false;
	saw_move=saw_hold_choose=saw_old_label=saw_respawn_label=false;
	respawn_rpcs = unload_rpcs = 0;
	owned_respawn_rpcs=leave_requests=leave_flushes=0;
	reject_rpc = false;
	current = &screen_death;
	gstate.autopilot.active = false;
	gstate.autopilot.death_screen_ticks = 0;
	gstate.players[0].current_screen = &screen_death;
	memset(forced_prev, 0, sizeof(forced_prev));
	click_edges = 0;
	local_player.health = 0;
	gstate.players[0].local_player = &local_player;
	gstate.players[0].respawn_failure=RESPAWN_FAILURE_NONE;
	gstate.players[0].respawn_request_observation_pending=false;
	gstate.players[0].host_respawn_loading=false;
	gstate.world_loaded = true;
	gstate.world_switch_pending = false;
	gstate.config_death_hold_respawn_ms = 750;
	gstate.config_death_hold_title_ms = 1500;
	screen_death.reset(&screen_death, view_w, view_h);
}

static void test_owned_guest_state(void) {
	struct client_session* host=&gstate.players[0];
	struct client_session* guest=&gstate.players[1];
	screen_death_owner_reset(host);
	struct entity guest_body={.health=0};
	*guest=(struct client_session){.local_bound=true,.local_ticket=7,
		.local_generation=19,.local_phase=CLIENT_GUEST_ATTACHED,
		.local_player=&guest_body,.local_gameplay_ready=true};
	screen_death_owner_enter(guest);
	assert(guest->death.active&&guest->death.selected==0);
	struct input_player_frame frame={.connected=true};
	/* Release clears only this owner's fresh-press latch and grace. */
	assert(screen_death_owner_update(guest,1,&frame,.5F,0,240,640,240));
	assert(guest->death.click_armed&&!host->death.active);
	frame.down[IB_GUI_CLICK]=frame.pressed[IB_GUI_CLICK]=true;
	for(int k=0;k<50;k++) {
		screen_death_owner_update(guest,1,&frame,.016F,0,240,640,240);
		frame.pressed[IB_GUI_CLICK]=false;
	}
	assert(owned_respawn_rpcs==1&&owned_source.slot==1
		&&owned_source.generation==19&&owned_rpc.type==SRPC_RESPAWN
		&&owned_rpc.payload.respawn.owner==1
		&&owned_rpc.payload.respawn.generation==19
		&&owned_rpc.payload.respawn.respawn_epoch==0);
	assert(guest->death.commit_pending==DEATH_COMMIT_RESPAWN
		&&guest->death.request_generation==19
		&&guest->death.completed_floor==0&&cue_count==0);
	/* Wrong generation and the captured floor cannot commit or cue. */
	guest->local_generation=20;guest->local_respawn_completed_epoch=1;
	screen_death_owner_update(guest,1,&frame,.016F,0,240,640,240);
	assert(guest->death.active&&cue_count==0);
	guest->local_generation=19;guest->local_respawn_completed_epoch=0;
	screen_death_owner_update(guest,1,&frame,.016F,0,240,640,240);
	assert(guest->death.active&&cue_count==0);
	guest->local_respawn_completed_epoch=1;
	screen_death_owner_update(guest,1,&frame,.016F,0,240,640,240);
	assert(!guest->death.active&&cue_count==1&&cues[0]==MENU_SOUND_SELECT);
	guest_body.health=20;
	assert(!screen_death_owner_update(guest,1,&frame,.016F,0,240,640,240)
		&&!guest->death.active&&cue_count==1);

	/* A primary-frame direction never changes owner 1; its own frame does. */
	guest_body.health=0;guest->local_respawn_completed_epoch=1;
	screen_death_owner_enter(guest);
	struct input_player_frame primary={.connected=true};
	primary.pressed[IB_GUI_DOWN]=true;
	screen_death_owner_enter(host);
	pressed[IB_GUI_DOWN]=true;
	screen_death_owner_update(host,0,&primary,.5F,0,0,640,240);
	pressed[IB_GUI_DOWN]=false;
	assert(host->death.selected==1&&guest->death.selected==0);
	frame=(struct input_player_frame){.connected=true,.pointer_valid=true,
		.pointer_x=100,.pointer_y=300,.pointer_angle=.25F};
	screen_death_owner_update(guest,1,&frame,.5F,0,240,640,240);
	assert(guest->death.selected==0&&guest->death.pointer_available
		&&guest->death.pointer_x==100&&guest->death.pointer_y==60
		&&guest->death.pointer_angle==.25F);
	gradient_quads=glyph_calls=0;
	screen_death_owner_render(guest,1,640,240);
	assert(gradient_quads==1&&gradient_v[0]==0&&gradient_v[1]==0
		&&gradient_v[6]==640&&gradient_v[7]==240&&glyph_calls>0
		&&prompt_owner==1);
	frame.pointer_valid=false;
	frame.pressed[IB_GUI_DOWN]=true;
	screen_death_owner_update(guest,1,&frame,.016F,0,240,640,240);
	assert(guest->death.selected==1&&primary.pressed[IB_GUI_DOWN]);

	/* Both cards accumulate their own hold time in the same logical frame. */
	screen_death_owner_reset(host);screen_death_owner_reset(guest);
	screen_death_owner_enter(host);screen_death_owner_enter(guest);
	memset(pressed,0,sizeof(pressed));memset(held,0,sizeof(held));
	pointer_on=false;
	frame=(struct input_player_frame){.connected=true};
	screen_death_owner_update(host,0,&primary,.5F,0,0,640,240);
	screen_death_owner_update(guest,1,&frame,.5F,0,240,640,240);
	held[IB_GUI_CLICK]=true;pressed[IB_GUI_CLICK]=true;
	frame.down[IB_GUI_CLICK]=frame.pressed[IB_GUI_CLICK]=true;
	screen_death_update_viewport(.2F,640,240);
	screen_death_owner_update(guest,1,&frame,.4F,0,240,640,240);
	assert(host->death.hold_s>.19F&&host->death.hold_s<.21F
		&&guest->death.hold_s>.39F&&guest->death.hold_s<.41F);
	memset(guest,0,sizeof(*guest));
	puts("  split death: owner-1 frame and exact owned respawn mailbox are isolated");
}

static void step(float dt) {
	screen_death.update(&screen_death, dt);
}

/* Hold IB_GUI_CLICK for `ms`, in 16 ms frames. */
static void hold_for(int ms) {
 bool continuing=held[IB_GUI_CLICK];held[IB_GUI_CLICK]=true;pressed[IB_GUI_CLICK]=!continuing;
 for(int t=0;t<ms;t+=16){step(.016F);pressed[IB_GUI_CLICK]=false;}
}

/* One frame in main.c's order, with NO pressed[]/held[] at all: input_poll's
 * once-per-frame latch advance (input.c's input_autopilot_edges_advance),
 * then -- on a 20Hz boundary -- the autopilot tick that moves
 * death_screen_ticks, then the screen update that reads input_pressed. */
static void autopilot_step(float dt, bool tick) {
	for(int b = 0; b < IB_COUNT; b++) {
		bool forced = false, val;
		if(gstate.autopilot.active
		   && force_button((enum input_button)b, &val))
			forced = val;
		autopilot_force_edge_advance(forced_prev, (enum input_button)b, forced);
	}
	if(tick)
		gstate.autopilot.death_screen_ticks++;
	step(dt);
}

/* ---- 1. the preserved timing gates ------------------------------------- */

static void test_timing_gates(void) {
	assert(screen_death_test_grace_s() > 0.399f
		   && screen_death_test_grace_s() < 0.401f); /* 400 ms entry */

	/* A button held from combat can never fire: the latch needs a released
	 * frame first, and the 400 ms grace runs on top of it. */
	enter_death();
	hold_for(3000);
	assert(respawn_rpcs == 0 && unload_rpcs == 0);

	/* Fresh press, but released before 750 ms: no respawn, and SILENT --
	 * "Hold start, progress and cancelled hold are silent." */
	enter_death();
	held[IB_GUI_CLICK] = false;
	step(0.016f); /* observe released -> armed */
	for(int t = 0; t < 500; t += 16)
		step(0.016f); /* burn the 400 ms grace */
	cue_count = 0;
	hold_for(600);
	held[IB_GUI_CLICK] = false;
	step(0.016f);
	assert(respawn_rpcs == 0);
	assert(cue_count == 0);

	/* Held past 750 ms: the respawn RPC goes out. */
	hold_for(800);
	assert(respawn_rpcs == 1);

	/* The exit row needs the longer 1,500 ms hold. */
	enter_death();
	held[IB_GUI_CLICK] = false;
	step(0.016f);
	for(int t = 0; t < 500; t += 16)
		step(0.016f);
	pressed[IB_GUI_DOWN] = true;
	step(0.016f);
	pressed[IB_GUI_DOWN] = false;
	hold_for(800);
	assert(unload_rpcs == 0); /* 750 ms is not enough for the exit row */
	hold_for(800);
	assert(unload_rpcs == 1);
	for(size_t k=0;k<cue_count;k++)assert(cues[k]!=MENU_SOUND_BACK&&cues[k]!=MENU_SOUND_SELECT);
	puts("  death: 400 ms grace, fresh-press latch, 750/1500 ms holds "
		 "preserved 1/1");
}

/* ---- 2. confirmation only on commit ------------------------------------ */

static bool saw(enum menu_sound_event e) {
	for(size_t k = 0; k < cue_count; k++)
		if(cues[k] == e)
			return true;
	return false;
}

static void test_commit_timing(void) {
	/* Pure predicate: only SERVER-observable state commits a respawn. */
	assert(!screen_death_test_respawn_committed(false, true, false));
	assert(!screen_death_test_respawn_committed(true, false, false));
	assert(screen_death_test_respawn_committed(false, false, true));
	assert(screen_death_test_respawn_committed(true, true, false));

	enter_death();
	held[IB_GUI_CLICK] = false;
	step(0.016f);
	for(int t = 0; t < 500; t += 16)
		step(0.016f);
	cue_count = 0;
	hold_for(800);
	assert(respawn_rpcs == 1);
	/* The RPC is out and the arm is live, but the world has not answered --
	 * NOTHING has been played. This is the whole point. */
	assert(screen_death_test_commit_armed());
	assert(!saw(MENU_SOUND_SELECT));

	held[IB_GUI_CLICK] = false;
	for(int t = 0; t < 2000; t += 16)
		step(0.016f); /* many frames of waiting: still silent */
	assert(!saw(MENU_SOUND_SELECT));
	assert(screen_death_test_commit_armed());

	/* The server answers: a world switch begins. NOW the confirmation lands,
	 * exactly once. */
	gstate.world_switch_pending = true;
	step(0.016f);
	assert(saw(MENU_SOUND_SELECT));
	assert(!screen_death_test_commit_armed());
	size_t after = cue_count;
	step(0.016f);
	step(0.016f);
	assert(cue_count == after); /* never a second confirmation */

	/* A rejected send arms nothing at all, so a later commit-looking world
	 * state cannot play a cue for an operation that never started. */
	enter_death();
	held[IB_GUI_CLICK] = false;
	step(0.016f);
	for(int t = 0; t < 500; t += 16)
		step(0.016f);
	reject_rpc = true;
	cue_count = 0;
	hold_for(800);
	assert(respawn_rpcs == 0 && !screen_death_test_commit_armed());
	reject_rpc = false;
	gstate.world_switch_pending = true;
	step(0.016f);
	assert(!saw(MENU_SOUND_SELECT));
	puts("  death: respawn confirmation lands only on the server's commit, "
		 "once, and never for a rejected send 1/1");
}

static void click_selected_once(void) {
	held[IB_GUI_CLICK]=false;pressed[IB_GUI_CLICK]=false;step(.5F);
	pressed[IB_GUI_CLICK]=held[IB_GUI_CLICK]=true;step(.016F);
	pressed[IB_GUI_CLICK]=held[IB_GUI_CLICK]=false;
}

static void test_split_host_protocol_and_exit(void) {
	static struct entity guest_body;
	enter_death();
	gstate.world.dimension=WORLD_DIM_OVERWORLD;
	gstate.players[1]=(struct client_session){.local_bound=true,
		.local_gameplay_ready=true,.local_player=&guest_body};
	gstate.config_death_hold_respawn_ms=0;
	click_selected_once();
	struct client_session* host=&gstate.players[0];
	assert(respawn_rpcs==1&&host->death.host_local_respawn
		&&host->death.commit_pending==DEATH_COMMIT_RESPAWN
		&&host->respawn_request_observation_pending);
	/* Health is not the split-host completion edge. */
	local_player.health=20;step(.016F);
	assert(host->death.active&&!cue_count);
	host->host_respawn_completed_epoch=1;step(.016F);
	assert(!host->death.active&&cue_count==1&&cues[0]==MENU_SOUND_SELECT);

	/* A typed failure retires only the pending press and never cues/closes. */
	enter_death();gstate.world.dimension=WORLD_DIM_OVERWORLD;
	gstate.players[1]=(struct client_session){.local_bound=true,
		.local_gameplay_ready=true,.local_player=&guest_body};
	gstate.config_death_hold_respawn_ms=0;click_selected_once();
	host->respawn_failure=RESPAWN_FAILURE_RETRY;
	host->respawn_failure_serial++;
	step(.016F);
	assert(host->death.active&&host->death.commit_pending==DEATH_COMMIT_NONE
		&&!cue_count);
	host->respawn_failure=RESPAWN_FAILURE_TERMINAL;
	click_selected_once();
	assert(respawn_rpcs==1); /* terminal disables Respawn; exit remains usable */
	pressed[IB_GUI_DOWN]=true;step(.016F);pressed[IB_GUI_DOWN]=false;
	gstate.config_death_hold_title_ms=0;click_selected_once();
	assert(unload_rpcs==1&&host->death.exit_pending
		&&host->death.commit_pending==DEATH_COMMIT_EXIT&&current==&screen_death);
	/* A respawn completion racing the pending unload cannot dismiss/cue it. */
	host->host_respawn_completed_epoch++;
	step(.016F);assert(host->death.active&&host->death.exit_pending
		&&cue_count==1); /* focus only */
	assert(screen_death_owner_exit_settled(host,false));
	assert(host->death.active&&host->death.exit_failed&&current==&screen_death);
	host->death.exit_pending=true;
	assert(screen_death_owner_exit_settled(host,true));
	assert(current==&screen_title&&!host->death.active
		&&cues[cue_count-1]==MENU_SOUND_BACK);

	/* Manual FAILED remains exit-only even if a discarded READY/health reply
	 * made the retained body look alive. Row 0 cannot enqueue Respawn. */
	struct client_session* guest=&gstate.players[1];
	guest_body.health=MAX_PLAYER_HEALTH;
	*guest=(struct client_session){.local_bound=true,.local_ticket=4,
		.local_generation=5,.local_phase=CLIENT_GUEST_FAILED,
		.local_player=&guest_body,.local_gameplay_ready=true,
		.local_death_leave_manual=true,.death={.active=true}};
	gstate.config_death_hold_respawn_ms=0;
	struct input_player_frame frame={.connected=true};
	assert(screen_death_owner_update(guest,1,&frame,.5F,0,240,640,240));
	frame.down[IB_GUI_CLICK]=frame.pressed[IB_GUI_CLICK]=true;
	assert(screen_death_owner_update(guest,1,&frame,.016F,0,240,640,240));
	assert(owned_respawn_rpcs==0&&guest->death.active);
	memset(guest,0,sizeof(*guest));
	puts("  death: split host health-before-READY, typed failure, pending exit and authoritative settlement 1/1");
}


/* Local Save and quit owns the death pane until the server settles unload.
 * Exercise the real UI and settlement callback; transport is doubled above.
 * CRPC_RETURN_INGAME dispatches false, menu WORLD_RESET dispatches true. */
static void test_solo_save_exit_settlement(void) {
	struct client_session* host=&gstate.players[0];
	for(int cycle=0;cycle<2;cycle++) {
		gstate.remote_session=false;
		gstate.players[1]=(struct client_session){0};
		enter_death();
		gstate.world.dimension=WORLD_DIM_OVERWORLD;
		pressed[IB_GUI_DOWN]=true;step(.016F);pressed[IB_GUI_DOWN]=false;
		gstate.config_death_hold_title_ms=0;
		cue_count=0;
		/* A full action queue cannot create an exit owner or hide the corpse. */
		reject_rpc=true;click_selected_once();
		assert(!unload_rpcs&&!host->death.exit_pending
			&&host->death.active&&current==&screen_death&&!cue_count);
		reject_rpc=false;click_selected_once();
		assert(unload_rpcs==1&&host->death.exit_pending
			&&host->death.commit_pending==DEATH_COMMIT_EXIT
			&&host->death.active&&current==&screen_death&&!cue_count);
		/* Fresh repeated presses cannot queue more unloads while saving. */
		click_selected_once();click_selected_once();
		assert(unload_rpcs==1&&host->death.exit_pending&&!cue_count);
		assert(screen_death_owner_exit_settled(host,false));
		assert(!host->death.exit_pending&&host->death.exit_failed
			&&host->death.active&&current==&screen_death&&!cue_count);
		/* Failure is retryable; a retry remains on the same pane. */
		click_selected_once();
		assert(unload_rpcs==2&&host->death.exit_pending
			&&!host->death.exit_failed&&current==&screen_death&&!cue_count);
		assert(screen_death_owner_exit_settled(host,true));
		assert(current==&screen_title&&!host->death.active
			&&!host->death.exit_pending&&cue_count==1&&cues[0]==MENU_SOUND_BACK);
		assert(!screen_death_owner_exit_settled(host,true)&&cue_count==1);
	}
	/* A pending local respawn owns the operation even before its reply.
	 * Exit must not overwrite it or enqueue an unload the server drops. */
	gstate.remote_session=false;enter_death();
	gstate.config_death_hold_respawn_ms=0;click_selected_once();
	assert(respawn_rpcs==1&&host->death.commit_pending==DEATH_COMMIT_RESPAWN);
	pressed[IB_GUI_DOWN]=true;step(.016F);pressed[IB_GUI_DOWN]=false;
	gstate.config_death_hold_title_ms=0;cue_count=0;click_selected_once();
	assert(!unload_rpcs&&!host->death.exit_pending
		&&host->death.commit_pending==DEATH_COMMIT_RESPAWN&&current==&screen_death);
	/* Typed refusal retires respawn ownership and restores the exit choice. */
	host->respawn_failure=RESPAWN_FAILURE_RETRY;host->respawn_failure_serial++;
	step(.016F);assert(host->death.commit_pending==DEATH_COMMIT_NONE);
	click_selected_once();assert(unload_rpcs==1&&host->death.exit_pending);
	assert(screen_death_owner_exit_settled(host,false));
	/* Loading barriers remain owners even with no pending UI press. */
	host->host_respawn_loading=true;click_selected_once();
	assert(unload_rpcs==1&&!host->death.exit_pending);
	host->host_respawn_loading=false;gstate.world_switch_pending=true;
	click_selected_once();assert(unload_rpcs==1&&!host->death.exit_pending);
	gstate.world_switch_pending=false;
	/* Remote Leave keeps the existing enqueue-to-title behavior: unlike a
	 * local save, its graceful disconnect sends no world-reset reply. */
	gstate.remote_session=true;enter_death();
	pressed[IB_GUI_DOWN]=true;step(.016F);pressed[IB_GUI_DOWN]=false;
	gstate.config_death_hold_title_ms=0;cue_count=0;click_selected_once();
	assert(unload_rpcs==1&&current==&screen_title
		&&!host->death.exit_pending&&!cue_count);
	gstate.remote_session=false;
	puts("  death: solo save refusal/retry/commit, queue rejection, pending input and remote Leave 1/1");
}

/* ---- 3. scroll on changed selection ------------------------------------ */

static void test_selection_cue(void) {
	enter_death();
	cue_count = 0;
	pressed[IB_GUI_DOWN] = true;
	step(0.016f);
	assert(cue_count == 1 && cues[0] == MENU_SOUND_FOCUS);
	step(0.016f); /* already on row 1: unchanged, therefore silent */
	assert(cue_count == 1);
	pressed[IB_GUI_DOWN] = false;
	pressed[IB_GUI_UP] = true;
	step(0.016f);
	assert(cue_count == 2 && cues[1] == MENU_SOUND_FOCUS);
	puts("  death: scroll cue only on a CHANGED selection 1/1");
}

/* ---- 4. affected-viewport composition ---------------------------------- */

static void test_viewport_composition(void) {
	/* Single player: the gradient covers the whole screen in Beta's exact
	 * colours (which are byte-identical to the approved prototype's). */
	gstate.players[1].local_player = NULL;
	gstate.players[1].local_gameplay_ready = false;
	enter_death();
	screen_death.render2D(&screen_death, view_w, view_h);
	assert(gradient_quads == 1);
	assert(gradient_v[0] == 0 && gradient_v[1] == 0);
	assert(gradient_v[3] == view_w && gradient_v[7] == view_h);
	assert(gradient_c[0] == 0x50 && gradient_c[1] == 0x00
		   && gradient_c[2] == 0x00 && gradient_c[3] == 0x60);
	assert(gradient_c[8] == 0x80 && gradient_c[9] == 0x30
		   && gradient_c[10] == 0x30 && gradient_c[11] == 0xA0);
	/* critique-1028: two footer glyph cells (D-pad + A), none inside the
	 * buttons; plain centred labels; "Move" / "Hold to choose". */
	assert(glyph_calls==2 && yellow_score);
	assert(saw_move && saw_hold_choose && saw_respawn_label && !saw_old_label);

	/* Admitted split: the gradient covers the AFFECTED viewport only, so the
	 * other player's half is untouched -- "Other players' menus/world state
	 * are not reset." */
	struct entity second;
	memset(&second, 0, sizeof(second));
	gstate.players[1].local_player = &second;
	gstate.players[1].local_gameplay_ready = true;
 view_h=240;
	enter_death();
	screen_death.render2D(&screen_death, view_w, view_h);
	assert(gradient_quads == 1);
	assert(gradient_v[7] == 240);
 view_h=480;

	/* Not admitted (a present but unready P2): the full composition stands. */
	gstate.players[1].local_gameplay_ready = false;
	enter_death();
	screen_death.render2D(&screen_death, view_w, view_h);
	assert(gradient_v[7] == view_h);
	gstate.players[1].local_player = NULL;

	/* Rows stay inside the viewport in both compositions. */
	for(int h = 240; h <= 480; h += 240) {
		struct menu_composition c;
		menu_composition_begin(&c, MENU_SCREEN_DEATH, 0, 0, 640, h);
		for(int k = 0; k < 2; k++) {
			struct menu_rect r = menu_death_row(&c, k);
			assert(r.y >= 0 && r.y + r.h <= h);
			assert(r.x >= 0 && r.x + r.w <= 640);
		}
	}
	puts("  death: gradient in native colours fills the AFFECTED viewport, "
		 "gated by P2 admission 1/1");
}

/* ---- 5. host / guest exit label ---------------------------------------- */

static void test_exit_label(void) {
	gstate.remote_session = false;
	assert(strcmp(screen_death_test_exit_label(), "Save and quit") == 0);
	gstate.remote_session = true;
	assert(strcmp(screen_death_test_exit_label(), "Leave game") == 0);
	gstate.remote_session = false;
	puts("  death: host save-and-quit vs guest leave 1/1");
}


void input_prompt_frame(unsigned player,struct input_player_frame* frame){
 assert(player<2);prompt_owner=player;memset(frame,0,sizeof *frame);frame->glyphs[IB_GUI_CLICK]=17;frame->glyphs[IB_GUI_UP]=18;
}
bool input_glyph_cells(int glyph,int* modifier,int* button){if(glyph<0)return false;*modifier=glyph/INPUT_GLYPH_CELLS-1;*button=glyph%INPUT_GLYPH_CELLS;return true;}
int gutil_prompt_glyph_width(int glyph,int size){int m,b;if(!input_glyph_cells(glyph,&m,&b))return 0;return m>=0?size*2+size/2:size;}
int gutil_prompt_glyph(int x,int y,int size,int glyph){(void)x;(void)y;assert(size==24&&(glyph==17||glyph==18));glyph_calls++;return 24;}
/* The A1 hint row (10-31): a headless stand-in that keeps each item whose
 * role has a glyph, and draws its glyph through gutil_prompt_glyph. */
bool gutil_hint_row_layout(const struct input_player_frame* input, int x,
	int y, int width, int height, bool split, enum gutil_hint_align align,
	const struct gutil_hint_item* items, int count, struct gutil_hint_row* out){
 (void)x;(void)y;(void)width;(void)height;(void)split;(void)align;
 memset(out,0,sizeof *out);
 for(int k=0;k<count&&out->count<GUTIL_HINT_ROW_MAX;k++){
  int g=input->glyphs[items[k].roles[0]];if(g<0)continue;
  struct gutil_hint_cell* c=&out->cells[out->count++];
  c->item=k;c->glyph_count=1;c->glyph[0]=g;c->label=items[k].label;
 }
 out->strip.width=out->count?1:0;
 return out->count>0;
}
void gutil_hint_row_draw(const struct gutil_hint_row* row){
 for(int k=0;k<row->count;k++){
  (void)gutil_prompt_glyph(0,0,GUTIL_HINT_GLYPH,row->cells[k].glyph[0]);
  gutil_text(0,0,row->cells[k].label,GUTIL_HINT_TEXT,true);
 }
}
void gutil_text_rgb(int x,int y,const char* str,int size,bool shadow,uint8_t r,uint8_t g,uint8_t b){
 (void)x;(void)y;(void)size;(void)shadow;if(!strcmp(str,"0")&&r==255&&g==255&&b==85)yellow_score=true;
 if(!strcmp(str,"Respawn")&&size==16)saw_respawn_label=true;
}
static void test_approved_geometry_and_pointer(void){
 for(int h=240;h<=480;h+=240){struct menu_composition c;menu_composition_begin(&c,MENU_SCREEN_DEATH,0,0,640,h);
 for(int k=0;k<2;k++){struct menu_rect r=menu_death_row(&c,k);
 assert(r.x==(h==480?150:74+k*252));assert(r.y==(h==480?180+k*50:120));assert(r.w==(h==480?340:240));assert(r.h==(h==480?40:36));}}
 enter_death();step(.5F);pointer_on=true;ptr_x=10;ptr_y=10;hold_for(1000);assert(!respawn_rpcs);
 ptr_x=160;ptr_y=190;hold_for(1000);assert(!respawn_rpcs); /* dragging an outside press in cannot arm */
 held[IB_GUI_CLICK]=false;step(.016F);hold_for(800);assert(respawn_rpcs==1);
}
static void test_split_pointer_hitbox(void) {
 enter_death();
 screen_death_update_viewport(.5F,640,240);
 pointer_on=true;ptr_x=100;ptr_y=260; /* Full pointer -> (100,130), split Respawn. */
 held[IB_GUI_CLICK]=true;pressed[IB_GUI_CLICK]=true;
 screen_death_update_viewport(.016F,640,240);
 pressed[IB_GUI_CLICK]=false;
 for(int i=0;i<50;i++)screen_death_update_viewport(.016F,640,240);
 assert(respawn_rpcs==1 && unload_rpcs==0);
 enter_death();screen_death_update_viewport(.5F,640,240);
 pointer_on=true;ptr_x=100;ptr_y=100; /* Outside both split rows. */
 held[IB_GUI_CLICK]=true;pressed[IB_GUI_CLICK]=true;
 for(int i=0;i<100;i++)screen_death_update_viewport(.016F,640,240);
 assert(!respawn_rpcs&&!unload_rpcs);
 puts("  split death: pointer and render dimensions agree; outside hold is inert");
}
/* ---- 6. the autopilot's forced click is a real PRESS -------------------- */

/* Regression for the real-Wii lan_walk_0912 FAIL ("mp autopilot walk displaced
 * 0.0 blocks"): the walker died ~10 s after login and never respawned. Since
 * the native-menus port, screen_death seeds its Respawn hold from
 * input_pressed(IB_GUI_CLICK) -- but input_pressed had no autopilot hook at
 * all (only input_held did), so autopilot.c's scripted held click was a LEVEL
 * with no edge, the hold never started, and the corpse stood there forever.
 * pressed[]/held[] stay empty throughout: every press below comes out of the
 * forcing alone. */
static void test_autopilot_forced_click_presses(void) {
	enter_death();
	gstate.autopilot.active = true; /* debug.autopilot=on, walk mode */

	/* The script's released first tick: the fresh-press latch arms and the
	 * 400 ms entry grace burns with the forced button UP. */
	for(int t = 0; t < 500; t += 16)
		autopilot_step(0.016f, false);
	assert(!input_held(IB_GUI_CLICK) && click_edges == 0 && !respawn_rpcs);

	/* death_screen_ticks 1 -> 2: the script starts holding. Exactly ONE press
	 * comes out of that level, and the 750 ms hold it seeds commits. Ticks
	 * advance at the real 20 Hz (every third 60 fps frame), so that hold runs
	 * to completion well before the script's next released tick at 1 + P. */
	gstate.autopilot.death_screen_ticks = 1;
	autopilot_step(0.016f, true);
	assert(click_edges == 1);
	autopilot_step(0.016f, false);
	assert(click_edges == 1); /* still forced: never a second activation */
	for(int f = 2; f < 52; f++)
		autopilot_step(0.016f, f % 3 == 0);
	assert(gstate.autopilot.death_screen_ticks
		   < AUTOPILOT_DEATH_CLICK_RETRY_TICKS); /* one period, one press */
	assert(click_edges == 1 && respawn_rpcs == 1 && unload_rpcs == 0);
	for(int k = 0; k < IB_COUNT; k++)
		assert(!pressed[k] && !held[k]); /* no real input was ever involved */

	/* The same script from a real entry (death_screen_ticks climbing from 0 at
	 * 20 Hz under 60 fps frames): the input layer produces exactly ONE edge
	 * per retry period, each on the single frame the forcing restarts -- never
	 * a run of edges out of the continuous level behind it. */
	enter_death();
	gstate.autopilot.active = true;
	unsigned produced = 0;
	int produced_at[4];
	for(int f = 0; f < 120; f++) {
		autopilot_step(0.016f, f % 3 == 0);
		bool forced;
		if(force_button(IB_GUI_CLICK, &forced)
		   && autopilot_force_edge_pressed(forced_prev, IB_GUI_CLICK, forced)) {
			assert(produced < 4);
			produced_at[produced++] = f;
		}
	}
	/* Two edges in 1.92 s: the 2nd 20 Hz tick, then the retry one period
	 * later (tick 1 + P, at 3 frames per tick). */
	assert(produced == 2);
	assert(produced_at[0] == 3);
	assert(produced_at[1] == 3 * AUTOPILOT_DEATH_CLICK_RETRY_TICKS + 3);
	/* MEASURED, and the whole reason the retry exists: the FIRST edge lands
	 * 48 ms into the screen (100 ms at a real 20 Hz tick) while screen_death's
	 * entry grace holds the gate shut for 400 ms -- the screen does not even
	 * ask for it (it short-circuits on gate_open), so that edge is lost and
	 * the level behind it can never start a hold. Only the retry edge arrives
	 * with the gate open, which is why click_edges counts exactly one press
	 * here, the second one, at 1.52 s. test_autopilot_respawns_end_to_end
	 * below carries that through to the commit. */
	assert(produced_at[0] * 16 < 400); /* inside the grace: unusable */
	assert(produced_at[1] * 16 > 400); /* past it: this is the one that works */
	assert(click_edges == 1);
	gstate.autopilot.active = false;
	puts("  death: an autopilot-FORCED IB_GUI_CLICK is a one-frame PRESS that "
		 "seeds the Respawn hold 1/1");
}

/* ---- 7. end-to-end: the scripted walker actually respawns --------------- */

/* The complete loop, with NOTHING shortened: the REAL 400 ms entry grace, the
 * REAL 750 ms default Respawn hold, autopilot.h's real retry period and the
 * real input-layer edge rule. This is the case that would have caught the
 * real-Wii lan_walk_0912 FAIL end to end -- F1 fixed the edge, but the walker
 * still never respawned because the only edge fell inside the grace.
 *
 * Frames run at the autopilot's own 20 Hz (dt 0.05, one tick per frame), the
 * worst case for the script: each released tick is visible to the screen for
 * exactly ONE frame, so if the grace/hold arithmetic were off by a tick this
 * case would not commit. */
static void test_autopilot_respawns_end_to_end(void) {
	enter_death();
	gstate.autopilot.active = true;

	/* Both durations are the real ones. */
	assert(screen_death_test_grace_s() > 0.399f
		   && screen_death_test_grace_s() < 0.401f);
	assert(gstate.config_death_hold_respawn_ms == 750);

	int commit_frame = -1, revive_frame = -1, left_at = -1;
	for(int f = 0; f < 100; f++) { /* up to 5 s of death-screen time */
		autopilot_step(0.05f, true);
		if(respawn_rpcs && commit_frame < 0) {
			commit_frame = f;
			assert(click_edges == 1); /* the retry press, and only it */
		}
		/* The server answers the committed respawn the way the real one does:
		 * the world switch begins (which is what resolves the cue), then the
		 * revived player settles and screen_death's backstop hands the frame
		 * back to the walk. */
		if(commit_frame >= 0 && f == commit_frame + 4)
			gstate.world_switch_pending = true;
		if(commit_frame >= 0 && f == commit_frame + 10) {
			local_player.health = 20;
			gstate.world_switch_pending = false;
			revive_frame = f;
		}
		if(current != &screen_death) {
			left_at = f;
			break;
		}
	}

	/* It respawned -- exactly once -- and not before the grace was over. */
	assert(respawn_rpcs == 1 && unload_rpcs == 0);
	assert(commit_frame >= 0);
	assert(commit_frame * 50 > 400); /* committed AFTER the entry grace */
	/* And it came from the retry edge (tick 1 + P), inside the uninterrupted
	 * hold that edge seeds -- not from the swallowed first one, and not from a
	 * later period. */
	assert(commit_frame > AUTOPILOT_DEATH_CLICK_RETRY_TICKS);
	assert(commit_frame < 2 * AUTOPILOT_DEATH_CLICK_RETRY_TICKS);
	assert(saw(MENU_SOUND_SELECT)); /* confirmation, only once the world moved */
	assert(revive_frame >= 0 && left_at == revive_frame + 1);
	assert(current == &screen_ingame);

	/* A further 3 s with the autopilot still running: the walker is back
	 * in-game, the script no longer owns IB_GUI_CLICK (autopilot_tick zeroes
	 * death_screen_ticks off the screen), and nothing activates a second time.
	 * The retry is deliberately unbounded WHILE the death screen is up --
	 * SRPC_RESPAWN is idempotent there by screen_death.c's own design -- so
	 * what has to end it is the commit closing the screen, and this is the
	 * assertion that it does. */
	gstate.players[0].current_screen = current;
	gstate.autopilot.death_screen_ticks = 0;
	unsigned edges_after = click_edges;
	for(int f = 0; f < 60; f++) {
		autopilot_step(0.05f, false);
		bool forced;
		assert(!force_button(IB_GUI_CLICK, &forced));
	}
	assert(respawn_rpcs == 1 && unload_rpcs == 0);
	assert(click_edges == edges_after);

	gstate.autopilot.active = false;
	puts("  death: autopilot_respawns_end_to_end -- real 400 ms grace + real "
		 "750 ms hold, one respawn, no repeat 1/1");
}

int main(void) {
	puts("approved death screen: commit timing and composition");
	test_approved_geometry_and_pointer();
	test_timing_gates();
	test_commit_timing();
	test_split_host_protocol_and_exit();
	test_solo_save_exit_settlement();
	test_selection_cue();
	test_viewport_composition();
	test_exit_label();
	test_split_pointer_hitbox();
	test_owned_guest_state();
	test_autopilot_forced_click_presses();
	test_autopilot_respawns_end_to_end();
	puts("PASS approved death commit timing");
	return 0;
}
