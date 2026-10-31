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

#include <assert.h>
#include "../../platform/input_glyph.h"

#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../network/server_interface.h"
#include "../../platform/controller_events.h"
#include "../../platform/gfx.h"
#include "../../platform/input_inventory.h"
#include "../../platform/input_owner.h"
#include "../game_state.h"
#include "menu_header.h"
#include "loading_tips.h"
#include "local_menu_host.h"

/* Approved loss/recovery behaviour (docs/ui_design/approved/specs/menus.md,
 * "Controller events"): pause single-player without closing or resetting its
 * current menu, STAY paused when the controller comes back, and resume only
 * on a fresh deliberate press once the device has reported neutral. The
 * previous version resumed the moment the connection returned -- which both
 * skipped the neutral gate and could act on a button still held from before
 * the loss. */
static struct controller_recovery primary_recovery;
static struct gutil_menu_focus recovery_focus;
/* The session kind the record above was taken under. A REMOTE modal holds
 * owns_pause without ever pausing the world (see the gates below), so a
 * record may never outlive the session kind that produced it: main.c:3402 /
 * :3500 flip gstate.remote_session on the provider start / teardown, and the
 * next frame's recovery step would otherwise let the new session's machine
 * claim -- and then ACT_RESUME away -- a pause the player opened themselves
 * (controller_events.c: "a pause the player opened themselves is never taken
 * away from them"). */
static bool recovery_session_remote;
/* A4 (10-31): the single-player modal is the shared disconnect card
 * (gui_util.h), full-screen size; Resume's hit rect is its layout's. */
static bool primary_recovery_layout(const struct input_player_frame* owner,int width,int height,
 struct controller_recovery_card* card,struct recovery_card_layout* l) {
 if(!owner||!controller_recovery_card(&primary_recovery,owner->category,false,card))return false;
 bool action=primary_recovery.phase==CONTROLLER_RECOVERY_READY&&card->action[0];
 recovery_card_layout(0,0,width,height,false,card,action,owner->glyphs[IB_GUI_CLICK],l);
 return true;
}
static struct gutil_button_bounds recovery_button(int width,int height) {
 struct controller_recovery_card card;struct recovery_card_layout l;
 if(!primary_recovery_layout(input_player_frame(0),width,height,&card,&l))return (struct gutil_button_bounds){0,0,0,0};
 return l.button;
}
void screen_controller_recovery_render(int width,int height) {
 struct controller_recovery_card card;struct recovery_card_layout l;
 const struct input_player_frame* owner=input_player_frame(0);
 if(!primary_recovery_layout(owner,width,height,&card,&l))return;
 gfx_texture(false);gutil_texquad_col(0,0,0,0,0,0,width,height,0,0,0,153);gfx_texture(true);
 float mx,my,angle;bool pointer=input_pointer(&mx,&my,&angle);
 bool focus=l.has_button&&(!recovery_focus.pointer_owner||!pointer||gutil_button_hit(&l.button,mx,my));
 recovery_card_draw(&l,&card,owner->glyphs[IB_GUI_CLICK],focus);
 if(pointer){gfx_bind_texture(&texture_pointer);gutil_texquad_rt_any(mx,my,glm_rad(angle),0,0,256,256,48,48);}
}

const struct controller_recovery* screen_controller_recovery_state(void) {
	return &primary_recovery;
}

bool screen_controller_recovery(void) {
	const struct input_player_frame* owner = input_player_frame(0);
	struct input_device device;
	/* Drop a record the new session kind cannot own (see the declaration).
	 * This is the single production entry for the machine (main.c:4402, every
	 * frame), so the drop happens on the first frame after the switch and
	 * before the step below reads owns_pause. */
	if(recovery_session_remote != gstate.remote_session) {
		recovery_session_remote = gstate.remote_session;
		primary_recovery = (struct controller_recovery) {0};
		recovery_focus = (struct gutil_menu_focus){.initialized=true};
	}
	/* Remote recovery keeps its modal ownership without pausing the world.
	 * The recovery machine uses paused to distinguish a deliberate dismissal
	 * from an external resume, so feed it the modal's own remote gate. */
	struct controller_recovery_input in = {
		.paused = gstate.paused || (gstate.remote_session && primary_recovery.owns_pause)
	};
	struct controller_recovery next;
	enum controller_recovery_action action;
	if(!gstate.players[0].local_player || !owner
	   || gstate.players[0].current_screen == &screen_title) {
		primary_recovery = (struct controller_recovery) {0};
		recovery_focus = (struct gutil_menu_focus){.initialized=true};
		return false;
	}
	/* Split-screen loss is the affected player's own compact recovery and
	 * must not stop the shared world (menus.md: "local pause does not pretend
	 * the world is stopped"). That composition belongs to the split menu
	 * work, not to this single-player gate -- so this gate stands down, and
	 * hands back any pause it was holding rather than leaving the shared world
	 * stopped with no gate left to resume it. */
	if(gstate.players[1].local_player && gstate.players[1].local_bound
	   && gstate.players[1].local_gameplay_ready) {
		if(!gstate.remote_session && primary_recovery.owns_pause && gstate.paused) {
			if(!svin_rpc_send(&(struct server_rpc) {.type = SRPC_TOGGLE_PAUSE}))
				return true;
			gstate.paused = false;
		}
		primary_recovery = (struct controller_recovery) {0};
		recovery_focus = (struct gutil_menu_focus){.initialized=true};
		return false;
	}

	const struct input_device_status* status
		= input_player_device(0, &device) ?
		input_inventory_device(input_devices(), device) :
		NULL;
	if(status) {
		bool usable = status->published.connected
			&& (status->published.category == INPUT_CAT_NUNCHUK
				|| status->published.category == INPUT_CAT_CLASSIC_CONTROLLER
				|| status->published.category == INPUT_CAT_GAMECUBE);
		in.present = usable;
		/* `armed` IS the neutral gate: it is cleared whenever the device
		 * stops being playable and only set again by a FRESH report in which
		 * every control of the owner's family rests. */
		in.neutral = status->armed;
		in.reason = status->published.connected ?
			CONTROLLER_EVENT_ATTACHMENT_MISSING :
			CONTROLLER_EVENT_DISCONNECTED;
	} else {
		/* No inventory entry (a keyboard/mouse owner): the frame's own
		 * connected flag is the only hardware signal there is. */
		in.present = owner->connected;
		in.neutral = owner->connected;
		in.reason = CONTROLLER_EVENT_DISCONNECTED;
	}
	/* The modal owns the whole update, leaving the underlying menu/focus intact.
	 * Pointer clicks must hit an action; a generic Confirm on the retained menu
	 * must not resume or quit behind the dialog. */
 bool gated=primary_recovery.phase!=CONTROLLER_RECOVERY_NONE;
 bool resume_hit=true;
 if(gated&&owner->connected&&primary_recovery.phase==CONTROLLER_RECOVERY_READY) {
  float x=0,y=0,angle=0;bool pointer=input_pointer(&x,&y,&angle);
  bool direction=owner->pressed[IB_GUI_UP]||owner->pressed[IB_GUI_DOWN]||owner->pressed[IB_GUI_LEFT]||owner->pressed[IB_GUI_RIGHT];
  if(gutil_menu_pointer_focus(&recovery_focus,pointer,x,y,direction)) {
   struct gutil_button_bounds q=recovery_button(gfx_width(),gfx_height());resume_hit=gutil_button_hit(&q,x,y);
  }
 }
 in.resume=gated&&owner->connected&&primary_recovery.phase==CONTROLLER_RECOVERY_READY&&owner->pressed[IB_GUI_CLICK]&&resume_hit;

	action = controller_recovery_step(&primary_recovery, in, &next);
	bool sent = true;
	if(!gstate.remote_session && (action == CONTROLLER_RECOVERY_ACT_PAUSE
	   || action == CONTROLLER_RECOVERY_ACT_RESUME)) {
		sent = svin_rpc_send(&(struct server_rpc) {.type = SRPC_TOGGLE_PAUSE});
		if(sent) gstate.paused = action == CONTROLLER_RECOVERY_ACT_PAUSE;
	}
	if(sent && (action == CONTROLLER_RECOVERY_ACT_RESUME || action == CONTROLLER_RECOVERY_ACT_DISMISS)) {
		audio_menu_event(MENU_SOUND_BACK, time_get());
		recovery_focus = (struct gutil_menu_focus){.initialized=true};
	}
	controller_recovery_commit(&primary_recovery, &next, action, sent);
	gated |= primary_recovery.phase != CONTROLLER_RECOVERY_NONE;
	if(gated) {
		/* Also suppress gameplay consumers outside the screen update, and
		 * consume the Resume press until release. */
		for(int button = 0; button < IB_COUNT; button++)
			if(owner->down[button] || owner->pressed[button] || owner->released[button])
				input_player_block(0, button);
	}
	return gated;
}

// NULL until main() registers it (PC only, see screen_multiplayer_connect.c
// and screen.h's comment above the extern declaration).
struct screen* screen_multiplayer_connect = NULL;
// NULL until main() registers it, same shape/reason as the pointer above
// (see screen_multiplayer_setup.c and screen.h's comment).
struct screen* screen_multiplayer_setup = NULL;

/* Where the next frame's controller notices compose. A menu that owns an
 * approved header/notice composition (menu_header.h) publishes its resolved
 * rect from its own render2D; menus that do not keep the historical
 * whole-viewport corner. Reset every frame so a screen change can never leave
 * a stale mount behind. */
static struct menu_rect notice_mount;
static bool notice_mount_valid, notice_mount_header;

void screen_notice_compose(int x, int y, int w, int h, bool in_header) {
	notice_mount_header=in_header;
	notice_mount = (struct menu_rect) {x, y, w, h};
	notice_mount_valid = true;
}

static void screen_render2D_notices(struct screen* s, int width, int height) {
	notice_mount_valid = false;
	/* toast-1030: every menu but gameplay itself is a menu context, and the
	 * content's own text/art is recorded so an unmounted notice can be put
	 * where it covers no heading, tab or label. */
	gutil_notices_menu_context(s != &screen_ingame);
	gutil_text_log_begin();
	s->render2D_content(s, width, height);
	gutil_text_log_end();
	bool split = gstate.players[1].local_player != NULL;
 /* notice-spot-1031 (maintainer 10-31): a floating card is ALWAYS at the
  * one top-left anchor (gameplay's 24,16), nudged down below a heading under
  * it -- never a per-menu spot (Texture Packs' note row, Play game's header
  * field) and never another corner. Only the approved compact-header
  * composition (split panes, menu_header.c) still draws inside its header. */
 if(notice_mount_valid&&notice_mount_header) {
  gutil_connection_notices_in_header(0,split,notice_mount.x,notice_mount.y,notice_mount.w,notice_mount.h);
 } else {
  int card_w=gutil_connection_notice_width(0,split,false);
  if(card_w>0) {
   int nx=GUTIL_NOTICE_ANCHOR_X,ny=GUTIL_NOTICE_ANCHOR_Y;
   (void)gutil_notice_free_spot(width,height,card_w,GUTIL_NOTICE_CARD_H,&nx,&ny);
   gutil_connection_notices(0,split,nx-8,ny-8,card_w+16,GUTIL_NOTICE_CARD_H+16);
  }
 }
	gutil_notices_menu_context(false);
	screen_controller_recovery_render(width, height);
}

bool screen_split_container_base(uint8_t window,int width,int height) {
 if(!gstate.players[1].local_bound||!gstate.players[1].local_gameplay_ready
  ||!gstate.players[1].local_player||gstate.paused||!lworld_admit_gameplay())return false;
 if(!local_menu_host_open(&gstate.players[0],0,window,width,height))return false;
 gstate.players[0].menu_host.split_owned=true;
 /* No normal ingame reset: it would enable movement and disable the pointer
  * while this owner's authoritative container is still open. */
 gstate.players[0].current_screen=&screen_ingame;
 input_text_enable(false);
 if(gstate.players[0].local_player)gstate.players[0].local_player->data.local_player.capture_input=false;
 input_pointer_enable(true);return true;
}

void screen_set(struct screen* s) {
	assert(s);
	/* All gameplay entries, including RPC ACK/RETURN and menu recovery,
	 * share the loading screen's presentation gate. Rejected requests must
	 * not reset its elapsed time on every frame or repeated RPC. */
	if(s == &screen_ingame && !lworld_admit_gameplay()) {
		if(gstate.players[0].current_screen == &screen_load_world)
			return;
		s = &screen_load_world;
	}
	if(s != &screen_ingame && s->render2D
	   && s->render2D != screen_render2D_notices) {
		s->render2D_content = s->render2D;
		s->render2D = screen_render2D_notices;
	}
	/* Covers completion, cancellation and external disconnect/error routes. */
	if(gstate.players[0].current_screen == &screen_load_world
	   && s != &screen_load_world) loading_reason_end(0);
	/* Full -> compact bypasses screen_set (screen_split_container_base).
	 * Compact -> full selects screen_inventory. Every other exit from the
	 * full inventory retires the logical open, including external returns. */
	if(gstate.players[0].current_screen == &screen_inventory
	   && s != &screen_inventory)
		local_menu_host_inventory_closed(&gstate.players[0]);
	gstate.players[0].current_screen = s;
	if(s->reset)
		s->reset(s, gfx_width(), gfx_height());
}
