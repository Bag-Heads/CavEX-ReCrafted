/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Interim title-screen path (plan docs/mp_online_client_plan_2026-08-15.md
 * section 3.3): the minimal connecting/cancel/error screen. Reused pattern:
 * screen_load_world.c's progress screen, whose gstate.world_loaded /
 * world_switch_pending / world_load_failed trio it also watches, since
 * server_remote publishes world install through the SAME CRPCs
 * server_local does [plan 3.1].
 *
 * PC-only (server_remote is PC-only for this milestone -- S5 ports the Wii
 * transport). The whole real implementation below is compiled out under a
 * non-PLATFORM_PC build; screen_multiplayer_connect_impl still exists there
 * (inert, NULL callbacks) so main.c's registration line has something to
 * take the address of on every platform, but it is never wired up
 * (screen_multiplayer_connect stays NULL -- see screen.c) so screen_title.c
 * never reaches it. */

#include "screen.h"
/* Above the gate on purpose: server_remote.h defines CAVEWII_MP_CLIENT. */
#include "../../network/server_remote.h"

#ifdef CAVEWII_MP_CLIENT

#include <stdio.h>
#include <ctype.h>
#include <math.h>
#include <string.h>

#include "../../config.h"
#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../platform/audio.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "frontend_presentation.h"
#include "play_session.h"
#include "../game_state.h"
#include "../../network/mp_codec.h"
#include "../../network/client_interface.h" // clin_send_unload_world (chip-B retry sender)
#include "../../network/mp_session.h"
#include "../../network/server_interface.h"
#ifdef PLATFORM_WII
#include "../../platform/wii/net_bringup.h"
#endif

/* MPC_BRINGUP is Wii-only in practice (PC's WSAStartup is instant): the
 * console may not have an IP yet (~25s DHCP settle), and main.c's 15s
 * provider-swap deadline must NOT start until the connect request is
 * actually queued -- so bring-up runs BEFORE mp_session_request_connect
 * [S5 design doc section 2c, option 1]. */
enum mpc_state { MPC_BRINGUP, MPC_CONNECTING, MPC_ERROR };

static enum mpc_state mpc_state;
static char mpc_target[MP_ADDRESS_HOST_CAPACITY + 8]; /* "host:port" display */
static char mpc_error[256];
static bool mpc_ever_played;
/* The error page was entered from the mid-play disconnect latch (kick or drop
 * while in the world): title it "Disconnected", not a join failure. */
static bool mpc_disconnected_in_play;
static uint32_t mpc_baseline_generation;
static bool pointer_available;
static float pointer_x, pointer_y, pointer_angle;
/* Directional focus on the MPC_ERROR page: row 0 = Retry, row 1 = Back to
 * Join. Retry used to require `hover`, i.e. a pointer -- so on a Classic or
 * a GameCube pad a transient drop was a dead end (Astra Lane A item 1). */
static int mpc_focus;
static size_t mpc_reason_page, mpc_reason_lines;
static uint16_t mpc_reason_offsets[256], mpc_reason_lengths[256];
static char mpc_display_target[sizeof(mpc_target)];
static enum server_remote_state mpc_remote_stage;
static float mpc_activity_time;

/* A3 M-1: `pick_retry = hover || (!pointer_available && ...)` meant a Wii
 * Remote -- where a pointer is normally available -- never consulted
 * mpc_focus at all, so UP/DOWN did nothing and a player aiming off both
 * buttons could not reach Retry. Arbitrate instead, with the same
 * movement-based helper the other screens use. */
static struct gutil_menu_focus mpc_pointer_focus;
static bool mpc_pointer_owns;
/* WHO CHOSE the highlight -- see screen_title.c's note (lead ruling R-A,
 * closing review A6-1). Sticky: it moves only when the player does, never on
 * an IR validity transition. */
static enum gutil_menu_origin mpc_origin;
/* S4 disconnect UX: a mid-play kick/drop arrives from main.c's teardown seam
 * via screen_multiplayer_connect_show_disconnected BEFORE screen_set runs
 * this screen's reset -- the latch makes that reset display the reason
 * instead of immediately starting a fresh connect.
 *
 * The latch itself lives in gstate (remote_disconnect_requested/_reason,
 * game_state.h) rather than as a static here: screen_load_world.c's remote
 * stall/give-up escape hatch (task_db613f13) needs to arm this SAME latch
 * from the shared engine object pool, which deliberately does not link this
 * TU (CMakeLists.txt's CAVEX_MP_SOURCES comment -- keeps the socket
 * provider out of every unrelated PC unit test's link), so it cannot call
 * screen_multiplayer_connect_show_disconnected() below directly. It instead
 * writes gstate.remote_disconnect_requested/_reason itself and swaps
 * screens through the shared screen_multiplayer_connect POINTER (screen.h),
 * the exact same indirection screen_title.c already uses to reach this
 * screen from the shared pool. Both paths land here in mpc_reset(). */

void screen_multiplayer_connect_show_disconnected(const char* reason) {
	snprintf(gstate.remote_disconnect_reason,
			sizeof(gstate.remote_disconnect_reason), "%s",
			(reason && reason[0]) ? reason : "Connection lost");
	gstate.remote_disconnect_requested = true;
	screen_set(&screen_multiplayer_connect_impl);
}

/* The approved 640x480 composition; one geometry drives ink and hit tests. */
static int mpc_px(int v) { return v * gfx_height() / 480; }
static struct fep_rect mpc_rect(int x, int y, int w, int h) {
	return fep_rect((gfx_width() - mpc_px(640)) / 2 + mpc_px(x),
		mpc_px(y), mpc_px(w), mpc_px(h));
}
/* Fit a word-wrapped line, splitting long tokens instead of hiding them.
 * Source offsets retain every non-whitespace byte of the bounded native reason. */
static size_t mpc_line_end(const char* text, size_t start, int size, int room) {
	char probe[256];
	size_t end = start, space = start;
	while(text[end] && end - start + 1 < sizeof(probe)) {
		if(text[end] == '\n') return end;
		probe[end - start] = text[end]; probe[end - start + 1] = 0;
		if(gutil_font_width(probe, size) > room) break;
		if(isspace((unsigned char)text[end])) space = end;
		end++;
	}
	if(!text[end] || text[end] == '\n') return end;
	return space > start ? space : end > start ? end : start + 1;
}
static void mpc_layout_reason(void) {
	size_t at = 0;
	mpc_reason_lines = 0;
	while(mpc_error[at]) {
		while(isspace((unsigned char)mpc_error[at])) at++;
		if(!mpc_error[at]) break;
		size_t end = mpc_line_end(mpc_error, at, mpc_px(16), mpc_px(432));
		mpc_reason_offsets[mpc_reason_lines] = (uint16_t)at;
		mpc_reason_lengths[mpc_reason_lines++] = (uint16_t)(end - at);
		at = end;
	}
	if(mpc_reason_lines == 0) {
		mpc_reason_offsets[0] = mpc_reason_lengths[0] = 0;
		mpc_reason_lines = 1;
	}
	size_t pages = (mpc_reason_lines + 4) / 5;
	if(mpc_reason_page >= pages) mpc_reason_page = pages - 1;
}
static size_t mpc_reason_pages(void) { return (mpc_reason_lines + 4) / 5; }
/* 0 and 1 keep the existing Retry/Back focus ids. 2/3 page the reason. */
static struct fep_rect mpc_target_rect(int target) {
	if(mpc_state != MPC_ERROR) return target == 0 ? mpc_rect(248,332,144,44) : fep_rect(0,0,0,0);
	switch(target) {
	case 0: return mpc_rect(104,368,204,44);
	case 1: return mpc_rect(332,368,204,44);
	case 2: return mpc_reason_pages() > 1 ? mpc_rect(104,316,136,44) : fep_rect(0,0,0,0);
	case 3: return mpc_reason_pages() > 1 ? mpc_rect(400,316,136,44) : fep_rect(0,0,0,0);
	default: return fep_rect(0,0,0,0);
	}
}
static bool mpc_target_enabled(int target) {
	if(target == 0) return true;
	if(mpc_state != MPC_ERROR) return false;
	if(target == 1) return true;
	if(target == 2) return mpc_reason_page > 0;
	if(target == 3) return mpc_reason_page + 1 < mpc_reason_pages();
	return false;
}
static void mpc_return_to_join(void) {
	(void)play_session_set_tab(PLAY_TAB_JOIN);
	screen_set(screen_multiplayer_setup ? screen_multiplayer_setup : &screen_title);
}
static void mpc_remember_target_label(void) {
	const char* address = config_read_string(&gstate.config_user, "mp.server", "");
	const char* name = play_session_server_name();
	const char* label = name[0] && !strcmp(address, play_session_server_address()) ? name : address;
	if(!label[0] || (label == address && strlen(label) > 44)) label = "Direct connection";
	snprintf(mpc_display_target, sizeof(mpc_display_target), "%s", label);
}

static void mpc_begin_connect(void);

/* THE ONLY WAY INTO THE ERROR PAGE (review A16-5). MPC_ERROR is reached by
 * in-place state transitions from nine places -- the mid-play disconnect
 * handoff inside mpc_reset itself, four validation/queue failures in
 * mpc_begin_connect, and four outcomes in mpc_update -- and NONE of them runs
 * a second reset(). So the one-control CONNECTING page's sanctioned
 * DIRECTIONAL start (see mpc_reset's note) was leaking onto a page with TWO
 * buttons, where rule (2) then activated the ring's default, Retry, on a
 * press with the sprite over empty space. A9-7 asked for that asymmetry to be
 * confined to the one-control page; this is the confinement, stated once
 * instead of at nine call sites: the error page starts DEFAULT and takes
 * DIRECTIONAL from the player's first UP/DOWN, exactly like every other
 * screen with a ring. */
static void mpc_enter_error(void) {
	mpc_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	mpc_state = MPC_ERROR;
	mpc_focus = 0;
	mpc_reason_page = 0;
	mpc_layout_reason();
}

static void mpc_reset(struct screen* s, int width, int height) {
	(void)s;
	(void)width;
	(void)height;
	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;
	input_pointer_enable(true);
	pointer_available = false;
	mpc_focus = 0;
	/* A9-7 (and R-E). This page has ONE control -- Cancel -- and no ring to
	 * walk, so its selection can never acquire a DIRECTIONAL origin from a
	 * direction press the way every other screen's does; under R-E a DEFAULT
	 * origin would leave a pointer user resting on the background unable to
	 * press A at all, while rule (3) would still allow it with the pointer
	 * away. Review A9-7 asked for that asymmetry to be stated rather than
	 * inherited: it is stated here, and resolved -- on a one-control page an
	 * activate press can only mean that control, so the selection starts
	 * DIRECTIONAL. Cancel destroys nothing, and Close/HOME sit beside it
	 * doing the same thing.
	 *
	 * The ERROR page, which DOES have a ring, keeps DEFAULT and takes
	 * DIRECTIONAL from the first UP/DOWN -- and that is now TRUE (review
	 * A16-5). It used to be only a claim: this assignment is unconditional
	 * and MPC_ERROR is reached by in-place state changes with no second
	 * reset -- including from inside this very function, a few lines below --
	 * so the two-button error page ran DIRECTIONAL from frame one and rule (2)
	 * fired Retry on a click anywhere in the background. Every way into that
	 * page goes through mpc_enter_error() above, which stamps DEFAULT. */
	mpc_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	mpc_pointer_focus = (struct gutil_menu_focus) {0};
	mpc_pointer_owns = false;
	mpc_ever_played = false;
	mpc_disconnected_in_play = false;
	mpc_error[0] = '\0';
	mpc_activity_time = 0.0F;
	mpc_remote_stage = REMOTE_CONNECTING;
	mpc_remember_target_label();

	/* Mid-play disconnect handoff (see the latch comment above): display the
	 * reason, do NOT start a connect. Retry clears the latch and falls
	 * through to the normal connect path on the next reset. */
	if(gstate.remote_disconnect_requested) {
		gstate.remote_disconnect_requested = false;
		snprintf(mpc_error, sizeof(mpc_error), "%s",
				gstate.remote_disconnect_reason);
		mpc_enter_error();
		mpc_disconnected_in_play = true;
		/* Keep mpc_target in sync for the Retry path's display. */
		const char* server_cfg_display
			= config_read_string(&gstate.config_user, "mp.server", "");
		snprintf(mpc_target, sizeof(mpc_target), "%s", server_cfg_display);
		return;
	}

#ifdef PLATFORM_WII
	/* Gate the connect (and with it the provider-swap 15s clock) behind a
	 * live IP. Re-request is idempotent; after a FAILED verdict the Retry
	 * path re-enters here and restarts the ladder. */
	if(net_bringup_status(NULL, 0) != NET_BRINGUP_READY) {
		net_bringup_request();
		const char* server_cfg_display
			= config_read_string(&gstate.config_user, "mp.server", "");
		snprintf(mpc_target, sizeof(mpc_target), "%s", server_cfg_display);
		mpc_state = MPC_BRINGUP;
		return;
	}
#endif
	mpc_begin_connect();
}

/* The config-driven connect path (mp.server/mp.username, exactly the PC
 * flow): parse, validate, queue the request, enter MPC_CONNECTING. On Wii
 * this runs only once net_bringup reports READY (see mpc_reset/mpc_update)
 * so the provider-swap deadline downstream of the queued request can never
 * race DHCP. */
static void mpc_begin_connect(void) {
	const char* server_cfg
		= config_read_string(&gstate.config_user, "mp.server", "");
	const char* username
		= config_read_string(&gstate.config_user, "mp.username", "");
	/* No join without a chosen name (mp_codec.h mp_username_chosen): the
	 * setup screen's gate normally catches this first; a hand-edited
	 * config or a legacy "CaveWii" default lands here with a visible
	 * reason, never a silent join under a bot-looking name. */
	if(!mp_username_chosen(username)) {
		snprintf(mpc_error, sizeof(mpc_error),
				"Choose a player name in Multiplayer first");
		mpc_enter_error();
		return;
	}

	char host[MP_ADDRESS_HOST_CAPACITY];
	uint16_t port;
	if(!mp_address_parse(server_cfg, host, sizeof(host), &port)
	   || host[0] == '\0') {
		snprintf(mpc_error, sizeof(mpc_error),
				"Invalid mp.server address: \"%s\"", server_cfg);
		mpc_enter_error();
		return;
	}
	/* Packet1's 16-code-unit login maximum is stricter than Packet2's
	 * handshake -- rejecting here at connect time (visible error, no
	 * crash) instead of letting the handshake succeed and the login fail
	 * mid-connect. [740c583 Packet1Login.readPacketData] */
	if(!mp_login_username_valid(username)) {
		snprintf(mpc_error, sizeof(mpc_error),
				"mp.username \"%s\" is too long (16 character limit)",
				username);
		mpc_enter_error();
		return;
	}

	if(port == 25565)
		snprintf(mpc_target, sizeof(mpc_target), "%s", host);
	else
		snprintf(mpc_target, sizeof(mpc_target), "%s:%u", host,
				(unsigned)port);

	struct server_remote_status status;
	struct server_remote* remote = mp_session_remote();
	if(remote)
		server_remote_get_status(remote, &status);
	else
		memset(&status, 0, sizeof(status));
	mpc_baseline_generation = status.generation;
	/* Discard any stale drop notice from a PREVIOUS visit so it cannot
	 * insta-error this fresh attempt (re-review finding 3 wiring). */
	(void)mp_session_take_drop_notice(NULL, 0);

	if(!mp_session_request_connect(host, port, username)) {
		snprintf(mpc_error, sizeof(mpc_error),
				"Could not start connecting to %s", mpc_target);
		mpc_enter_error();
		return;
	}
	mpc_state = MPC_CONNECTING;
}

static float menu_motion_time;

static void mpc_update(struct screen* s, float dt) {
	if(dt > 0.0F && mpc_state != MPC_ERROR) menu_motion_time = fmodf(menu_motion_time + dt, 128.0F);
	(void)s;
	if(dt > 0.0F) mpc_activity_time = fmodf(mpc_activity_time + dt, 2.8F);
	if(mpc_state == MPC_ERROR) mpc_layout_reason();
	pointer_available = input_pointer(&pointer_x, &pointer_y, &pointer_angle);
	bool up = input_pressed(IB_GUI_UP), down = input_pressed(IB_GUI_DOWN);
	bool left = input_pressed(IB_GUI_LEFT), right = input_pressed(IB_GUI_RIGHT);
	bool previous = input_pressed(IB_GUI_PREVIOUS), next = input_pressed(IB_GUI_PAGE);
	bool navigating = mpc_state == MPC_ERROR && (up || down || left || right || previous || next);
	mpc_pointer_owns = gutil_menu_pointer_focus(&mpc_pointer_focus,
		pointer_available, pointer_x, pointer_y, navigating);
	int hovered = GUTIL_MENU_TARGET_NONE;
	for(int i = 0; pointer_available && i < 4; i++) {
		if(fep_rect_hit(mpc_target_rect(i), pointer_x, pointer_y)) { hovered = i; break; }
	}
	if(mpc_pointer_owns && hovered >= 0) {
		mpc_focus = hovered; mpc_origin = GUTIL_MENU_ORIGIN_POINTER;
	}
	if(navigating) {
		mpc_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
		if(mpc_state == MPC_ERROR) {
			if(previous && !next && mpc_reason_page > 0) mpc_reason_page--;
			if(next && !previous && mpc_reason_page + 1 < mpc_reason_pages()) mpc_reason_page++;
			if(up && !down) mpc_focus = mpc_reason_pages() > 1 && mpc_focus < 2 ? mpc_focus + 2 : 0;
			if(down && !up) mpc_focus = mpc_focus >= 2 ? mpc_focus - 2 : 1;
			if(left && !right) mpc_focus = mpc_focus >= 2 ? 2 : 0;
			if(right && !left) mpc_focus = mpc_focus >= 2 ? 3 : 1;
		}
	}
	bool click = input_pressed(IB_GUI_CLICK);
	struct gutil_menu_decision decision = gutil_menu_activate_decide((struct gutil_menu_activate_state) {
		.pressed = click, .navigated = navigating, .pointer_valid = pointer_available,
		.selection_origin = mpc_origin, .hovered = hovered,
		.hovered_enabled = mpc_target_enabled(hovered), .selected = mpc_focus,
		.selected_enabled = mpc_target_enabled(mpc_focus),
	});
	mpc_focus = decision.selected; mpc_origin = decision.selection_origin;
	if(click && decision.action != GUTIL_MENU_ACTIVATE) return;
	if(mpc_state == MPC_ERROR) {
		if(decision.action == GUTIL_MENU_ACTIVATE) {
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
			if(decision.target == 0) mpc_reset(s, gfx_width(), gfx_height());
			else if(decision.target == 1) { mp_session_cancel_pending(); mpc_return_to_join(); }
			else if(decision.target == 2) mpc_reason_page--;
			else if(decision.target == 3) mpc_reason_page++;
			return;
		}
		if(input_pressed(IB_INVENTORY) || input_pressed(IB_HOME) || input_pressed(IB_GUI_CLICK_ALT)) {
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
			mp_session_cancel_pending(); mpc_return_to_join();
		}
		return;
	}
	bool back = decision.action == GUTIL_MENU_ACTIVATE
		|| input_pressed(IB_INVENTORY) || input_pressed(IB_HOME) || input_pressed(IB_GUI_CLICK_ALT);
	if(back) {
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		/* Finding 3: a request reset() queued but main() has not drained yet
		 * must die with this screen -- otherwise it fires later, teardown
		 * and all, with no connect UI showing. */
		mp_session_cancel_pending();
#ifdef PLATFORM_WII
		if(mpc_state == MPC_BRINGUP)
			net_bringup_cancel(); /* advisory; the ladder stops next check */
#endif
		if(mpc_state == MPC_CONNECTING) {
			/* Same boundary-local RPC screen_pause.c's "quit to title" and
			 * screen_death.c already use [plan 6.2]: server_remote's
			 * remote_process_srpc handles SRPC_UNLOAD_WORLD unconditionally,
			 * even mid-connect, and main()'s pump loop resumes server_local
			 * once teardown completes. */
			/* Via the retry-latched sender (chip-B pattern, 2026-08-31): a
			 * saturated lifecycle reserve must delay this unload, never drop
			 * it -- clin_update retries until accepted. */
			clin_send_unload_world();
		}
		mpc_return_to_join();
		return;
	}

#ifdef PLATFORM_WII
	if(mpc_state == MPC_BRINGUP) {
		char bringup_error[96];
		enum net_bringup_state nbs
			= net_bringup_status(bringup_error, sizeof(bringup_error));
		if(nbs == NET_BRINGUP_READY) {
			mpc_begin_connect(); /* swap clock starts only from here on */
		} else if(nbs == NET_BRINGUP_FAILED) {
			snprintf(mpc_error, sizeof(mpc_error), "%s",
					 bringup_error[0] ? bringup_error
									  : "network bring-up failed");
			mpc_enter_error();
		}
		return;
	}
#endif

	if(mpc_state != MPC_CONNECTING)
		return;

	struct server_remote* remote = mp_session_remote();
	if(!remote) {
		snprintf(mpc_error, sizeof(mpc_error), "Multiplayer session unavailable");
		mpc_enter_error();
		return;
	}
	struct server_remote_status status;
	server_remote_get_status(remote, &status);

	/* main()'s loop has not drained/started our request yet this frame
	 * (mp_session_request_connect merely queued it) -- generation is still
	 * whatever it was before reset() asked. Wait rather than misreading a
	 * stale REMOTE_DISCONNECTED as "the new attempt already failed". */
	{
		/* Re-review finding 3: if main() consumed-and-DROPPED our request
		 * (provider busy), the generation never moves -- without this poll
		 * the screen spun "Connecting..." forever with no error and no
		 * timeout. */
		char drop_reason[64];
		if(mp_session_take_drop_notice(drop_reason, sizeof(drop_reason))) {
			snprintf(mpc_error, sizeof(mpc_error), "%s", drop_reason);
			mpc_enter_error();
			return;
		}
		if(status.generation == mpc_baseline_generation) return;
	}

	mpc_remote_stage = status.state;
	if(status.state == REMOTE_PLAY)
		mpc_ever_played = true;

	/* Mirrors screen_load_world.c's own transition exactly: server_remote
	 * publishes world install through the same CRPCs server_local does. */
	if(mpc_ever_played && gstate.world_loaded && !gstate.world_switch_pending
	   && !gstate.world_load_failed) {
		screen_set(&screen_ingame);
		return;
	}

	if(status.state == REMOTE_DISCONNECTED) {
		/* Fell back to disconnected without ever completing the world
		 * install above: report why. status.reason carries Packet255's
		 * kick text verbatim, or server_remote's own connect-failure
		 * message (DNS/refused/etc). [plan section 5 Packet255] */
		snprintf(mpc_error, sizeof(mpc_error), "%s",
				status.reason[0] ? status.reason : "Disconnected");
		mpc_enter_error();
	}
}

static void mpc_text(int x, int y, int size, int room, const char* text, bool dark, bool centered) {
	char fit[256]; fep_text_fit(fit, sizeof(fit), text, mpc_px(size), mpc_px(room));
	struct fep_rect r = mpc_rect(x,y,room,0);
	gutil_text_rgb(centered ? r.x + (r.w - gutil_font_width(fit,mpc_px(size))) / 2 : r.x,
		r.y,fit,mpc_px(size),!dark,dark?32:255,dark?32:255,dark?32:255);
}
static void mpc_draw_button(int target, const char* text) {
	struct fep_rect r = mpc_target_rect(target);
	bool enabled = mpc_target_enabled(target);
	/* The active pack already supplies the button bevel and focus edge. */
	fep_widget(r,enabled && mpc_focus == target);
	/* No glyph inside the button (controller UI redesign 10-31): the roles
	 * are named in the centred hint row (mpc_hint_row). */
	int font = mpc_px(16);
	char fitted[64];
	fep_text_fit(fitted,sizeof(fitted),text,font,r.w-mpc_px(16));
	gutil_text(r.x+(r.w-gutil_font_width(fitted,font))/2,r.y+(r.h-font)/2,fitted,font,true);
	if(!enabled) fep_fill(r,0,0,0,100);
}
static const char* mpc_stage_label(void) {
	if(mpc_state == MPC_BRINGUP) return "Bringing up network...";
	switch(mpc_remote_stage) {
	case REMOTE_HANDSHAKE_SENT: return "Waiting for server...";
	case REMOTE_LOGIN_SENT: return "Signing in...";
	case REMOTE_PLAY: return "Receiving world...";
	case REMOTE_KICKED: case REMOTE_CLOSING: return "Closing connection...";
	default: return "Contacting server...";
	}
}
/* A kick/drop during play keeps the server's own reason (Packet255 text, e.g.
 * a plugin's "Login Timeout") under "Disconnected", as Beta titles a kick:
 * [NetClientHandler.java:307 "new GuiConnectFailed("disconnect.disconnected",
 * "disconnect.genericReason", new Object[]{var1.reason})"]. Only a session
 * that never reached the world says it could not join. */
static const char* mpc_error_title(void) {
	return mpc_disconnected_in_play ? "Disconnected" : "Could not join server";
}
/* The centred A1 hint row (10-31): Try again / Page / Back on the error
 * page, Cancel while connecting. */
static void mpc_hint_row(int width, int height) {
	struct input_player_frame prompts;
	input_prompt_frame(0, &prompts);
	const struct gutil_hint_item error_items[] = {
		{{IB_GUI_CLICK}, 1, "Try again", 1, false},
		{{IB_GUI_PREVIOUS, IB_GUI_PAGE}, 2, "Page", 0, false},
		{{IB_INVENTORY}, 1, "Back", 2, true}};
	const struct gutil_hint_item busy_item = {{IB_INVENTORY}, 1, "Cancel", 0, true};
	struct gutil_hint_item items[3];
	int n = 0;
	if(mpc_state == MPC_ERROR) {
		items[n++] = error_items[0];
		if(mpc_reason_pages() > 1) items[n++] = error_items[1];
		items[n++] = error_items[2];
	} else items[n++] = busy_item;
	struct gutil_hint_row row;
	if(gutil_hint_row_layout(&prompts,0,0,width,height,false,
			GUTIL_HINT_ALIGN_CENTRE,items,n,&row))
		gutil_hint_row_draw(&row);
}
static void mpc_render2D(struct screen* s, int width, int height) {
	(void)s;
	if(!fep_backdrop_at(width, height, 90, menu_motion_time, mpc_state != MPC_ERROR)) gutil_bg();
	if(mpc_state == MPC_ERROR) {
		mpc_layout_reason();
		struct fep_rect panel = mpc_rect(80,108,480,256);
		fep_paper(panel); fep_bevel(panel);
		mpc_text(100,128,24,440,mpc_error_title(),true,true);
		for(size_t row = 0; row < 5 && mpc_reason_page*5+row < mpc_reason_lines; row++) {
			size_t line = mpc_reason_page*5+row, len = mpc_reason_lengths[line];
			char text[256]; memcpy(text,mpc_error+mpc_reason_offsets[line],len); text[len]=0;
			mpc_text(104,186+(int)row*22,16,432,text,true,false);
		}
		if(mpc_reason_pages() > 1) {
			char count[32]; snprintf(count,sizeof(count),"%u / %u",(unsigned)mpc_reason_page+1,(unsigned)mpc_reason_pages());
			mpc_draw_button(2,"Previous"); mpc_draw_button(3,"Next");
			mpc_text(244,328,15,152,count,true,true);
		}
		mpc_draw_button(0,"Try again"); mpc_draw_button(1,"Back");
	} else {
		/* Two fitted lines of the actual saved name or short direct address. */
		size_t at = 0;
		for(int row = 0; row < 2 && mpc_display_target[at]; row++) {
			while(isspace((unsigned char)mpc_display_target[at])) at++;
			size_t end = mpc_line_end(mpc_display_target,at,mpc_px(22),mpc_px(432));
			char text[256]; size_t len = end-at;
			memcpy(text,mpc_display_target+at,len); text[len]=0;
			mpc_text(104,170+row*26,22,432,text,false,true); at=end;
		}
		mpc_text(104,232,17,432,mpc_stage_label(),false,true);
		/* A moving 12px activity block, never a percentage or claimed work. */
		float phase = mpc_activity_time / 1.4F;
		if(phase > 1.0F) phase = 2.0F-phase;
		fep_fill(mpc_rect(296+(int)(36.0F*phase),278,12,12),220,220,220,255);
		mpc_draw_button(0,"Cancel");
	}
	mpc_hint_row(width,height);
	if(pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(pointer_x,pointer_y,glm_rad(pointer_angle),0,0,256,256,mpc_px(48),mpc_px(48));
	}
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* Read-only: which page this screen is on. tests/test_frontend_no_pointer.c
 * needs it to prove the ERROR page's Retry re-enters the connect path rather
 * than leaving the screen. Changes nothing. */
int screen_mpc_test_state(void) { return (int)mpc_state; }

/* Which error-page row directional focus rests on: 0 = Retry, 1 = Back. */
int screen_mpc_test_focus(void) { return mpc_focus; }
size_t screen_mpc_test_reason_page(void) { return mpc_reason_page; }
size_t screen_mpc_test_reason_pages(void) { mpc_layout_reason(); return mpc_reason_pages(); }
bool screen_mpc_test_target_rect(int target, struct gutil_button_bounds* out) {
	struct fep_rect r = mpc_target_rect(target);
	if(out) *out = (struct gutil_button_bounds) {r.x,r.y,r.w,r.h};
	return r.w > 0 && r.h > 0;
}
const char* screen_mpc_test_target_label(void) { return mpc_display_target; }
const char* screen_mpc_test_stage_label(void) { return mpc_stage_label(); }
const char* screen_mpc_test_error_title(void) { return mpc_error_title(); }
bool screen_mpc_test_reason_line(size_t line, char* out, size_t capacity) {
	mpc_layout_reason();
	if(line >= mpc_reason_lines || !out || capacity == 0) return false;
	size_t len = mpc_reason_lengths[line];
	if(len >= capacity) len = capacity - 1;
	memcpy(out,mpc_error+mpc_reason_offsets[line],len); out[len]=0;
	return true;
}

/* Put the screen on a given page. The rig doubles mp_session_request_connect
 * to fail, so MPC_CONNECTING -- whose Cancel review A6-2 is about -- is
 * otherwise unreachable headlessly.
 *
 * It sets the ORIGIN each page is really entered with, and must: with A16-5
 * the two pages no longer agree (the one-control CONNECTING / BRINGUP pages
 * keep mpc_reset's DIRECTIONAL carve-out, the two-button ERROR page starts
 * DEFAULT), so a hook that moved only mpc_state would hand a case a
 * combination the product cannot produce -- a CONNECTING page carrying the
 * error page's origin, which then reads as an unusable Cancel that no real
 * player can ever meet. Nothing else is written. */
void screen_mpc_test_set_state(int state) {
	if((enum mpc_state)state == MPC_ERROR) {
		mpc_enter_error();
		return;
	}
	mpc_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	mpc_state = (enum mpc_state)state;
}
#endif

struct screen screen_multiplayer_connect_impl = {
	.reset = mpc_reset,
	.update = mpc_update,
	.render2D = mpc_render2D,
	.render3D = NULL,
	.render_world = false,
};

#else /* !CAVEWII_MP_CLIENT */

/* No MP client on this platform; the symbol exists so main.c's
 * registration line can take its address unconditionally. */
struct screen screen_multiplayer_connect_impl = {0};

#endif
