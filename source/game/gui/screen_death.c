/*
	Copyright (c) 2022 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../network/server_interface.h"
#include "../../platform/audio.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "../../platform/menu_sound.h"
#include "../../platform/time.h"
#include "../game_state.h"
#include "menu_header.h"
#include "screen.h"
#include "screen_death.h"
#include "../../platform/input_owner.h"

/* Beta GuiGameOver, cited from [740c583]:
 *   initGui: GuiButton(1, width/2-100, height/4+72, "Respawn"),
 *            GuiButton(2, width/2-100, height/4+96, "Title menu")
 *   keyTyped {}                  -- swallows EVERY key; no way off the screen
 *                                   except the two buttons
 *   doesGuiPauseGame() == false  -- the world keeps ticking underneath
 *   drawScreen: drawGradientRect(0,0,w,h, 0x60500000, 0xA0803030);
 *     glScalef(2,2,2) + "Game over!" centered at (w/2/2, 30), 0xFFFFFF;
 *     "Score: &e" + getScore() centered at (w/2, 100), 0xFFFFFF.
 *     The '&' is LITERAL in b1.7.3 -- this version has no &->0xA7
 *     translation -- and the score is provably always 0
 *     (EntityLiving.scoreValue is 0 for every mob; nothing else calls
 *     addToPlayerScore with a nonzero value; not persisted), so the line
 *     renders exactly "Score: &e0", famous Beta quirk included.
 *   actionPerformed id 1: thePlayer.respawnPlayer()  -> our SRPC_RESPAWN
 *                   id 2: changeWorld1(null) + GuiMainMenu -> our
 *                         SRPC_UNLOAD_WORLD + screen_title.
 *
 * APPROVED DEATH COMPOSITION -- docs/ui_design/approved/menus.html (menu
 * selector -> Death) and specs/menus.md:
 *
 *   "Death covers the full single-player screen or affected split viewport,
 *    with the existing gradient and the revised layout. Preserve 400 ms entry
 *    protection, 750 ms Respawn hold and 1,500 ms exit hold from the approved
 *    prototype; completed native operations decide success. Other players'
 *    menus/world state are not reset."
 *
 * The prototype's gradient is `rgba(80,0,0,.3765)` -> `rgba(128,48,48,.6275)`,
 * which is byte-for-byte Beta's own 0x60500000 -> 0xA0803030 -- so the NATIVE
 * colours below are already the approved ones and are left untouched. What
 * this lane adds:
 *   1. the gradient and the layout follow the AFFECTED VIEWPORT rather than
 *      the whole framebuffer, gated by the same P2 acceptance predicate the
 *      furnace/dispenser split composition uses (menu_header.h);
 *   2. the confirmation cue is COMMIT-TIMED (see the sound note below);
 *   3. the exit row carries the approved host/guest label ("Save and quit" /
 *      "Leave game") instead of Beta's "Title menu" -- carve-out #2, same
 *      ownership rule as the pause menu; the RPC pair is unchanged.
 *
 * SOUND -- specs/sound-events.md, "Death screen": "No new death stinger.
 * Existing scroll on changed selection. Hold start, progress and cancelled
 * hold are silent. Confirmation only when respawn actually commits. ... exit
 * success cue is only after save/leave succeeds ... Never indicate save
 * success early."  The pre-approval code played Beta's quarter-volume
 * random.click at ACTIVATION -- i.e. before the server had done anything --
 * which is exactly the "false-success audio" the runtime rules forbid. The
 * activation now only ARMS a cue; death_commit_poll below plays it when the
 * respawn is observed committed by the server (a world switch, or a live
 * player on a settled world). Nothing is played on hold start, progress or a
 * cancelled hold.
 *
 * Input is IR-pointer + D-pad dual-drive per the Wiimote floor rule; the
 * D-pad selection renders as Beta's hover state (texture row + text color).
 */

/* [740c583 GuiGameOver.initGui]: both buttons are always the default
 * fixed-height GuiButton, width 200 -- an EVEN width, load-bearing for
 * gutil_button_draw_bg's half-split (gutil_button_even_width). */
#define DEATH_BTN_W MENU_DEATH_BUTTON_UNITS

/* Controller adaptation (carve-out #2; documented in the #18 entry): the
 * buttons fire on a HOLD with a visible fill sweep, not a bare click. On PC
 * the attack button IS the GUI-click button (both mouse code 1000), and the
 * screen appears mid-combat-mash -- a bare click would buy an accidental
 * respawn/quit the instant the screen opens, recreating deviation #18's
 * feel. Three additive layers, all feel-layer only (SRPC_RESPAWN and the
 * server transaction are untouched):
 *   1. entry GRACE  -- clicks are ignored for DEATH_GRACE_S after entry;
 *   2. fresh-press LATCH -- the button must be seen RELEASED after entry
 *      before a press can arm (a held-from-combat button never fires);
 *   3. HOLD -- the armed press must be held for the configured duration
 *      (gui.death_hold_respawn_ms 750 / gui.death_hold_title_ms 1500;
 *      0 restores the faithful Beta single click). Early release hard-
 *      resets; switching targets resets.
 * The approved prototype preserves all three values, so they are UNCHANGED by
 * this lane -- only the cue timing moved. */
#define DEATH_GRACE_S 0.4f

/* Which committed operation a cue is owed to. Armed at activation, resolved
 * only by an authoritative completion. */
/* PURE commit predicate, so the "never early" half of the rule is provable
 * without a GL context (test_death_commit_timing drives the real screen, and
 * this predicate is the thing it is proving). A respawn is committed when the
 * SERVER acts on it: either a world switch has begun (CRPC_BEGIN_WORLD_SWITCH)
 * or the player is alive again on a settled client world. Neither can be
 * produced by the local press. */
static bool death_respawn_committed(bool alive, bool world_loaded,
									bool switch_pending) {
	return switch_pending || (alive && world_loaded);
}

static bool death_is_host(void) {
	return !gstate.remote_session;
}

static const char* death_exit_label(unsigned player) {
	return player ? "Leave game" : (death_is_host() ? "Save and quit" : "Leave game");
}

/* The caller supplies the affected viewport; never halve it again here. */
static void death_compose(struct menu_composition* c, int width, int height) {
 menu_composition_begin(c, MENU_SCREEN_DEATH, 0, 0, width, height);
	/* No header: the approved prototype's `inHeader` rule excludes death, so
	 * a notice always keeps the floating card in the reserved top band. */
	for(int k = 0; k < 2; k++) {
		struct menu_rect r = menu_death_row(c, k);
		menu_composition_action(c, r.x, r.y, r.w, r.h);
	}
	menu_composition_seal(c);
}

static void screen_death_reset(struct screen* s, int width, int height) {
	(void)s;
	(void)width;
	(void)height;
	/* The corpse takes no movement input (Beta zeroes the dead player's
	 * moveStrafing/moveForward); the screen is pointer + D-pad driven like
	 * the container screens. */
	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;
	input_pointer_enable(true);
	screen_death_owner_reset(&gstate.players[0]);
	screen_death_owner_enter(&gstate.players[0]);
}

void screen_death_owner_reset(struct client_session* owner) {
	if(owner) owner->death = (struct death_ui){0};
}

void screen_death_owner_enter(struct client_session* owner) {
	if(!owner || owner->death.active) return;
	owner->death = (struct death_ui){.active=true,
		.grace_s=DEATH_GRACE_S, .hold_target=0};
}

/* Per-button hold requirement in seconds; 0 = the faithful single click. */
static float screen_death_hold_needed_s(int idx) {
	int ms = idx == 0 ? gstate.config_death_hold_respawn_ms
					  : gstate.config_death_hold_title_ms;
	return (float)ms / 1000.0f;
}

static void screen_death_activate(struct client_session* owner,
	unsigned player, int idx) {
	struct death_ui* death=&owner->death;
	if(idx == 0) {
		if(player&&owner->local_death_leave_manual)return;
		if(death->commit_pending==DEATH_COMMIT_RESPAWN
		   ||owner->local_respawn_loading||owner->host_respawn_loading
		   ||owner->respawn_failure==RESPAWN_FAILURE_TERMINAL
		   ||death->exit_pending)return;
		/* GuiGameOver id 1: respawnPlayer(). The transaction runs on the
		 * server thread; the screen stays up until CRPC_BEGIN_WORLD_SWITCH
		 * swaps in the loading screen (or the health>0 backstop in update
		 * fires). SRPC_RESPAWN is idempotent while dead, so a dropped send or
		 * a retryable server failure simply re-arms on the next press.
		 * NO CUE HERE -- the arm is resolved by death_respawn_committed. */
		bool accepted,host_local=false;
		if(player) {
			struct server_rpc_source source={.slot=1,
				.generation=owner->local_generation};
			struct server_rpc rpc={.type=SRPC_RESPAWN,
				.payload.respawn={.owner=1,
					.generation=owner->local_generation,
					.respawn_epoch=0}};
			accepted=owner->local_bound&&owner->local_generation
				&&svin_rpc_send_from(source,&rpc);
		} else {
			struct client_session* guest=&gstate.players[1];
			host_local=!gstate.remote_session
				&&gstate.world.dimension==WORLD_DIM_OVERWORLD
				&&guest->local_bound&&guest->local_player
				&&(guest->local_gameplay_ready||guest->local_respawn_loading);
			accepted=svin_rpc_send(&(struct server_rpc){.type=SRPC_RESPAWN,
				.payload.respawn={.owner=0,.generation=0,.respawn_epoch=0}});
		}
		if(accepted) {
			if(!player)death->host_local_respawn=host_local;
			death->commit_pending=DEATH_COMMIT_RESPAWN;
			death->request_generation=player?owner->local_generation:0;
			death->completed_floor=player?owner->local_respawn_completed_epoch
				:owner->host_respawn_completed_epoch;
			death->failure_serial=owner->respawn_failure_serial;
			owner->respawn_request_observation_pending=true;
		}
		return;
	}
	if(death->exit_pending)return;
	/* Respawn owns the host handoff until it commits or reports failure.
	 * The server rejects unload during that handoff; retain its UI owner. */
	if(!player&&!gstate.remote_session
	   &&(death->commit_pending==DEATH_COMMIT_RESPAWN
	   ||owner->host_respawn_loading||gstate.world_switch_pending))return;
	if(player) {
		if(client_session_try_local_leave(owner)) {
			death->exit_pending=true;death->exit_failed=false;
			death->commit_pending=DEATH_COMMIT_EXIT;
		} else death->exit_failed=true;
		return;
	}
	/* GuiGameOver id 2: changeWorld1(null) + GuiMainMenu. The unload saves
	 * the world WITH the dead player (health<=0 round-trips level.dat), so
	 * reopening that world faithfully re-enters this screen -- exactly Beta's
	 * quit-while-dead behaviour. No pause toggle first (unlike the pause
	 * menu's quit): the dead world was never paused,
	 * doesGuiPauseGame()==false. */
	if(svin_rpc_send(&(struct server_rpc) {.type = SRPC_UNLOAD_WORLD})) {
		/* Every local save is fallible. RETURN_INGAME retains this pane;
		 * the menu WORLD_RESET alone settles a successful unload. */
		if(!gstate.remote_session) {
			death->exit_pending=true;death->exit_failed=false;
			death->commit_pending=DEATH_COMMIT_EXIT;
			return;
		}
		/* A remote graceful Leave sends no reset completion reply. Preserve
		 * its immediate title transition without a success cue. */
		death->commit_pending = DEATH_COMMIT_NONE;
		screen_set(&screen_title);
	}
	/* else: the ordinary-action queue was full; stay here, retryable, silent. */
}

/* Resolve an armed respawn cue against authoritative world state. Returns true
 * when a cue was played this call. */
static bool death_commit_poll(struct client_session* owner, unsigned player,
	bool alive, bool world_loaded, bool switch_pending) {
	struct death_ui* death=&owner->death;
	if(death->commit_pending != DEATH_COMMIT_RESPAWN)
		return false;
	if(owner->respawn_failure_serial!=death->failure_serial) {
		death->failure_serial=owner->respawn_failure_serial;
		death->commit_pending=DEATH_COMMIT_NONE;
		return false;
	}
	bool committed=player
		? death->request_generation==owner->local_generation
			&&owner->local_respawn_completed_epoch>death->completed_floor
		: death->host_local_respawn
			? owner->host_respawn_completed_epoch>death->completed_floor
			: death_respawn_committed(alive,world_loaded,switch_pending);
	if(!committed)
		return false;
	if(death->exit_pending)return false;
	death->commit_pending = DEATH_COMMIT_NONE;
	death->active=false;
	/* MENU_SOUND_SELECT: "committed action succeeded". */
	audio_menu_event(MENU_SOUND_SELECT, time_get());
	return true;
}

/* Loading can replace the death screen before its next update. Completion
 * must still retire death.active, which gates host physics and camera ticks.
 * Poll only the existing authoritative commit; never dispatch hidden UI input. */
void screen_death_host_respawn_poll(void) {
 struct client_session* owner=&gstate.players[0];
 bool alive=owner->local_player&&owner->local_player->health>0;
 (void)death_commit_poll(owner,0,alive,gstate.world_loaded,
                        gstate.world_switch_pending);
 /* A refused exit can retain death.active while a prior handoff finishes
  * behind the loading pane. A settled live body with no pending operation
  * no longer owns a death presentation or its movement gate. */
 if(owner->death.active&&!owner->death.exit_pending
    &&owner->death.commit_pending==DEATH_COMMIT_NONE&&alive
    &&gstate.world_loaded&&!gstate.world_switch_pending
    &&!owner->host_respawn_loading)owner->death.active=false;
}

bool screen_death_owner_exit_settled(struct client_session* owner,bool success) {
	if(!owner||!owner->death.exit_pending)return false;
	struct death_ui* death=&owner->death;
	death->exit_pending=false;
	death->commit_pending=DEATH_COMMIT_NONE;
	if(success) {
		audio_menu_event(MENU_SOUND_BACK,time_get());
		death->active=false;
		if(owner==&gstate.players[0])screen_set(&screen_title);
	} else {
		death->exit_failed=true;
		death->active=true;
	}
	return true;
}

void screen_death_update_viewport(float dt, int width, int height) {
	struct client_session* owner=&gstate.players[0];
	bool alive = owner->local_player&&owner->local_player->health > 0;
	/* Poll BEFORE the leave below, so the committed respawn's cue is emitted
	 * while this screen still owns the frame. */
	if(death_commit_poll(owner,0,alive,gstate.world_loaded,
	   gstate.world_switch_pending))return;

	/* Respawn completed: Beta closes GuiGameOver after the respawn
	 * [740c583 Minecraft.respawn:1472-1474]. Normally the loading screen has
	 * already replaced this one (CRPC_BEGIN_WORLD_SWITCH); this backstop
	 * covers any flow that revives without a switch, gated on a settled
	 * client world exactly like the RETURN_INGAME reveal. */
	if(alive && !owner->death.host_local_respawn
	   && gstate.world_loaded && !gstate.world_switch_pending) {
		screen_set(&screen_ingame);
		return;
	}

	struct input_player_frame legacy={.connected=true};
	legacy.pointer_valid=input_pointer(&legacy.pointer_x,&legacy.pointer_y,
		&legacy.pointer_angle);
	legacy.down[IB_GUI_CLICK]=input_held(IB_GUI_CLICK);
	(void)screen_death_owner_update(owner,0,&legacy,dt,0,0,width,height);
}

bool screen_death_owner_update(struct client_session* owner, unsigned player,
	const struct input_player_frame* frame, float dt,
	int viewport_x, int viewport_y, int width, int height) {
	if(!owner||player>1)return false;
	screen_death_owner_enter(owner);
	struct death_ui* death=&owner->death;
	bool alive=owner->local_player&&owner->local_player->health>0;
	bool manual_failed=player&&owner->local_death_leave_manual
		&&owner->local_phase==CLIENT_GUEST_FAILED;
	if(death_commit_poll(owner,player,alive,gstate.world_loaded,
	   gstate.world_switch_pending))return false;
	if(player&&death->exit_pending&&!owner->local_respawn_loading
	   &&owner->local_phase!=CLIENT_GUEST_LEAVING) {
		death->exit_pending=false;death->commit_pending=DEATH_COMMIT_NONE;
		death->exit_failed=true;
	}
	if(player&&(!owner->local_bound||owner->local_phase==CLIENT_GUEST_IDLE)) {
		screen_death_owner_reset(owner);return false;
	}
	if(player&&!manual_failed&&!owner->local_respawn_loading
	   &&death->commit_pending==DEATH_COMMIT_NONE
	   &&(!owner->local_player||alive)) {
		screen_death_owner_reset(owner);return false;
	}
	if(player&&owner->local_phase!=CLIENT_GUEST_LEAVING
	   &&owner->local_phase!=CLIENT_GUEST_FAILED
	   &&(owner->local_phase!=CLIENT_GUEST_ATTACHED
	   ||!owner->local_generation||owner->local_reply_failed))return true;
	death->pointer_available=input_owner_pointer(frame,viewport_x,viewport_y,width,height,
		&death->pointer_x,&death->pointer_y,&death->pointer_angle);
	if(width<=0||height<=0) {death->hold_s=0;death->click_armed=false;return true;}
	/* P1's pointer covers its own viewport, as it does for split menus.
	 * Button hit boxes and drawing must use the same dimensions. */
	struct menu_composition c;
	death_compose(&c, width, height);
	int pointer_target=-1;
	if(death->pointer_available && death->hold_s==0.0F) {
		for(size_t k = 0; k < c.action_count; k++) {
			struct gutil_button_bounds bounds = {c.actions[k].x, c.actions[k].y,
												 c.actions[k].w,
												 c.actions[k].h};
			if(gutil_button_hit(&bounds, death->pointer_x, death->pointer_y)) {
    pointer_target=(int)k;
    if(death->selected==(int)k)continue;
				death->selected = (int)k;
				/* "Existing scroll on changed selection." */
				audio_menu_event(MENU_SOUND_FOCUS, time_get());
			}
		}
	}
	bool up=player ? frame&&(frame->pressed[IB_GUI_UP]||frame->pressed[IB_GUI_LEFT])
		: (input_pressed(IB_GUI_UP)||input_pressed(IB_GUI_LEFT));
	if(up && death->selected != 0) {
		death->selected = 0;
		audio_menu_event(MENU_SOUND_FOCUS, time_get());
	}
	bool down=player ? frame&&(frame->pressed[IB_GUI_DOWN]||frame->pressed[IB_GUI_RIGHT])
		: (input_pressed(IB_GUI_DOWN)||input_pressed(IB_GUI_RIGHT));
	if(down && death->selected != 1) {
		death->selected = 1;
		audio_menu_event(MENU_SOUND_FOCUS, time_get());
	}

	/* Beta keyTyped is an empty override: HOME and everything else is
	 * swallowed here -- deliberately no pause/inventory handling. The
	 * activation below runs the grace/latch/hold layers (header comment).
	 * Hold start, progress and a cancelled hold stay SILENT. */
	if(death->selected != death->hold_target) {
		death->hold_target = death->selected; /* retargeting never carries progress */
		death->hold_s = 0.0f;
	}
	if(death->grace_s > 0.0f)death->grace_s -= dt;
	if(!frame||!frame->down[IB_GUI_CLICK])death->click_armed = true;

	float need = screen_death_hold_needed_s(death->selected);
	bool gate_open = death->click_armed && death->grace_s <= 0.0f;
	if(need <= 0.0f) {
		/* Faithful Beta single click (hold disabled by config). */
		death->hold_s = 0.0f;
		bool pressed=gate_open&&(player ? frame&&frame->pressed[IB_GUI_CLICK]
			: input_pressed(IB_GUI_CLICK));
		if(gate_open && pressed && (!death->pointer_available||pointer_target==death->selected)) {
			death->click_armed = false;
			screen_death_activate(owner,player,death->selected);
		}
	} else if(gate_open && frame&&frame->down[IB_GUI_CLICK]
  && (death->hold_s>0.0F || ((player?frame->pressed[IB_GUI_CLICK]:input_pressed(IB_GUI_CLICK))
	      &&(!death->pointer_available||pointer_target==death->selected)))) {
		death->hold_s += dt;
		if(death->hold_s >= need) {
			death->hold_s = 0.0f;
			death->click_armed = false; /* any next action needs a fresh press */
			screen_death_activate(owner,player,death->selected);
		}
	} else {
		death->hold_s = 0.0f; /* early release: hard reset, no partial credit */
	}
	return true;
}


static void screen_death_update(struct screen* s,float dt) {
	(void)s;screen_death_update_viewport(dt,gfx_width(),gfx_height());
}

static int death_glyph_width(int glyph,int size) {
 return gutil_prompt_glyph_width(glyph,size); /* wide PC caps: 2x */
}
static void death_quad(int x,int y,int w,int h,int tx,int ty,int tw,int th,uint8_t alpha){
 gfx_draw_quads(4,(int16_t[]){x,y,-2,x+w,y,-2,x+w,y+h,-2,x,y+h,-2},
 (uint8_t[]){255,255,255,alpha,255,255,255,alpha,255,255,255,alpha,255,255,255,alpha},
 (uint16_t[]){tx,ty,tx+tw,ty,tx+tw,ty+th,tx,ty+th});
}
/* Untextured solid rectangle (the hold sweep and its progress line). */
static void death_rect(int x,int y,int w,int h,uint8_t r,uint8_t g,uint8_t b,
	uint8_t a){
	if(w<=0||h<=0)return;
	gfx_texture(false);
	gfx_draw_quads(4,(int16_t[]){x,y,-2,x+w,y,-2,x+w,y+h,-2,x,y+h,-2},
		(uint8_t[]){r,g,b,a,r,g,b,a,r,g,b,a,r,g,b,a},
		(uint16_t[]){0,0,0,0,0,0,0,0});
	gfx_texture(true);
}

/* critique-1028 death redesign (tmp/jobs/critique-1028/design-opus/NOTES.md
 * section 4, maintainer YES). Pure geometry so the test can pin it:
 *  - plain Beta buttons, label centred in the 16 px font (no "Hold" text and
 *    no glyph inside the button any more);
 *  - the hold sweep starts at the button's LEFT inner edge: x r.x+2 ..
 *    r.x+2+(r.w-4)*fill, y r.y+2 .. r.y+r.h-3, plus a 3 px progress line on
 *    the bottom inner edge over the same span;
 *  - footer "[D-pad] Move  [A] Hold to choose": the centred A1 hint row
 *    (10-31) -- 24 px cells, label 5 px after its cell, 14 px from a label's
 *    end to the next cell, the group centred; the cell row sits at y 430
 *    full screen and y 198 in a split pane (render draws the A1 row itself;
 *    this mirrors its geometry for the pure test).
 * Hold timing, activation and the glyph cells themselves are unchanged. */
void screen_death_layout(int width,int height,const int label_w[2],
	const float fill[2],int dpad_w,int a_w,int move_w,int hold_w,
	struct death_screen_layout* out){
	struct menu_composition c;death_compose(&c,width,height);
	bool full=c.layout==MENU_SCREEN_FULL;
	for(int k=0;k<2;k++){
		struct menu_rect r=c.actions[k];
		out->button[k]=r;
		out->label_x[k]=r.x+(r.w-label_w[k])/2;
		out->label_y[k]=r.y+(r.h-16)/2;
		float f=fill?fill[k]:0.0F;
		if(f<0)f=0;
		if(f>1)f=1;
		int fw=(int)((float)(r.w-4)*f);
		out->fill[k]=(struct menu_rect){r.x+2,r.y+2,fw,r.h-4};
		out->line[k]=(struct menu_rect){r.x+2,r.y+r.h-5,fw,3};
	}
	int group=dpad_w+DEATH_FOOTER_LABEL_GAP+move_w+DEATH_FOOTER_ITEM_GAP
		+a_w+DEATH_FOOTER_LABEL_GAP+hold_w;
	out->footer_w=group;
	out->footer_x=c.view.x+(c.view.w-group)/2;
	out->footer_y=c.view.y+(full?DEATH_FOOTER_Y_FULL:DEATH_FOOTER_Y_COMPACT);
	out->move_cell_x=out->footer_x;
	out->move_label_x=out->move_cell_x+dpad_w+DEATH_FOOTER_LABEL_GAP;
	out->hold_cell_x=out->move_label_x+move_w+DEATH_FOOTER_ITEM_GAP;
	out->hold_label_x=out->hold_cell_x+a_w+DEATH_FOOTER_LABEL_GAP;
}

void screen_death_owner_render(struct client_session* owner,unsigned player,
 int width,int height){
 if(!owner)return;
 struct death_ui* death=&owner->death;
 struct menu_composition c;death_compose(&c,width,height);
 int vx=c.view.x,vy=c.view.y,vw=c.view.w,vh=c.view.h;
 gfx_texture(false);
 gfx_draw_quads(4,(int16_t[]){vx,vy,-2,vx+vw,vy,-2,vx+vw,vy+vh,-2,vx,vy+vh,-2},
 (uint8_t[]){0x50,0,0,0x60,0x50,0,0,0x60,0x80,0x30,0x30,0xA0,0x80,0x30,0x30,0xA0},
 (uint16_t[]){0,0,0,0,0,0,0,0});gfx_texture(true);
 bool full=c.layout==MENU_SCREEN_FULL;int scale=full?32:28;
 gutil_text(vx+(vw-gutil_font_width("Game over!",scale))/2,vy+(full?68:44),"Game over!",scale,true);
 int score_x=vx+(vw-gutil_font_width("Score: 0",16))/2,score_y=vy+(full?122:82);
 gutil_text(score_x,score_y,"Score: ",16,true);
 gutil_text_rgb(score_x+gutil_font_width("Score: ",16),score_y,"0",16,true,255,255,85);
 struct input_player_frame prompts;input_prompt_frame(player,&prompts);
	const char* respawn=owner->local_death_leave_manual
		?"Respawn unavailable":owner->respawn_failure==RESPAWN_FAILURE_TERMINAL
		?"Respawn unavailable":owner->respawn_failure==RESPAWN_FAILURE_RETRY
		?"Retry Respawn":"Respawn";
	const char* exit_label=death->exit_failed
		?(player?"Retry Leave":"Retry Save and quit"):death_exit_label(player);
	const char* labels[]={respawn,exit_label};
 int dpad=prompts.glyphs[IB_GUI_UP],abtn=prompts.glyphs[IB_GUI_CLICK];
 int label_w[2]={gutil_font_width(labels[0],16),gutil_font_width(labels[1],16)};
 float fill[2];
 for(int k=0;k<2;k++){
  float need=screen_death_hold_needed_s(k);
  fill[k]=death->selected==k&&need>0?death->hold_s/need:0;
 }
 struct death_screen_layout L;
 screen_death_layout(width,height,label_w,fill,death_glyph_width(dpad,24),
  death_glyph_width(abtn,24),gutil_font_width(DEATH_FOOTER_MOVE,16),
  gutil_font_width(DEATH_FOOTER_HOLD,16),&L);
 for(int k=0;k<2;k++){
  struct menu_rect r=L.button[k];int half=r.w/2,src=half/GFX_GUI_SCALE,ty=death->selected==k?86:66;
  gfx_bind_texture(&texture_gui_widgets);
  death_quad(r.x,r.y,half,r.h,0,ty,src,20,255);
  death_quad(r.x+half,r.y,r.w-half,r.h,200-src,ty,src,20,255);
  /* The hold sweep: brighten (white a60) from the LEFT inner edge, plus the
   * #FFFFA0 progress line; an early release is fill 0 = nothing drawn. */
  death_rect(L.fill[k].x,L.fill[k].y,L.fill[k].w,L.fill[k].h,255,255,255,60);
  death_rect(L.line[k].x,L.line[k].y,L.line[k].w,L.line[k].h,255,255,160,255);
  bool sel=death->selected==k;
  gutil_text_rgb(L.label_x[k],L.label_y[k],labels[k],16,true,
   sel?255:224,sel?255:224,sel?160:224);
 }
 /* Footer: the centred A1 hint row in this view (10-31). */
 {
  static const struct gutil_hint_item items[]={
   {{IB_GUI_UP},1,DEATH_FOOTER_MOVE,0,false},
   {{IB_GUI_CLICK},1,DEATH_FOOTER_HOLD,1,true}};
  struct gutil_hint_row row;
  if(gutil_hint_row_layout(&prompts,vx,vy,vw,vh,!full,GUTIL_HINT_ALIGN_CENTRE,items,2,&row))
   gutil_hint_row_draw(&row);
 }
 screen_notice_compose(c.notice.x,c.notice.y,c.notice.w,c.notice.h,c.notice_in_header);
 if(death->pointer_available){gfx_bind_texture(&texture_pointer);gutil_texquad_rt_any(death->pointer_x,death->pointer_y,glm_rad(death->pointer_angle),0,0,256,256,48,48);}
}
static void screen_death_render2D(struct screen* s,int width,int height){
 (void)s;screen_death_owner_render(&gstate.players[0],0,width,height);
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
bool screen_death_test_commit_armed(void) {
	return gstate.players[0].death.commit_pending == DEATH_COMMIT_RESPAWN;
}
bool screen_death_test_respawn_committed(bool alive, bool world_loaded,
										 bool switch_pending) {
	return death_respawn_committed(alive, world_loaded, switch_pending);
}
float screen_death_test_grace_s(void) {
	return DEATH_GRACE_S;
}
const char* screen_death_test_exit_label(void) {
	return death_exit_label(0);
}
#endif

struct screen screen_death = {
	.reset = screen_death_reset,
	.update = screen_death_update,
	.render2D = screen_death_render2D,
	.render3D = NULL,
	.render_world = true,
};
