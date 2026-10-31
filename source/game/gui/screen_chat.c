/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

/* Multiplayer chat / command composer -- Beta's GuiChat on a Wii controller.
 *
 * Design record: docs/mp_chat_auth_research_2026-09-11.md ("Chat opener:
 * DECIDED", two Astra rounds). The EDIT SEMANTICS are Beta's [740c583
 * GuiChat]: an append-only draft capped at 100 characters, backspace removes
 * the last one, Enter trims and sends only a non-empty message as ONE
 * Packet3Chat (commands such as "/register <password>" are plain chat the
 * server interprets), ESC discards instantly with no prompt. The KEY SOURCE
 * is the second sanctioned carve-out (controller playability): the approved
 * sign-profile on-screen keyboard (widget_osk.h) inside the sign editor's
 * panel composition, with the sign button contract
 * (docs/ui_design/control-bindings.md): A = key, B = backspace,
 * Nunchuk Z / Classic Y / GC X = Send, Remote 1 / Classic X / GC Y = Cancel,
 * Home = Cancel. PC keeps Beta's physical keyboard through the OSK's text
 * queue (Enter = send, Esc = cancel).
 *
 * Only a REMOTE session ever reaches this screen (screen_ingame /
 * screen_pause gate the opener on gstate.remote_session): Beta SP's
 * EntityPlayer.sendChatMessage is an empty method, so the SP screen would be
 * dead, and split-screen has no online mode -- the composer is player 0's.
 *
 * Two documented deviations from GuiChat's LOOK (the behaviour above is
 * exact): (1) the open-chat history shows the lines that fit above the
 * keyboard (Beta shows 20 opaque lines; a 240-row TV cannot hold 20 lines
 * plus a keyboard), and (2) an overlong draft shows its TAIL instead of
 * running off the right edge as Beta's does. */

#include <stdio.h>
#include <string.h>

#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../network/server_interface.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_glyph.h"
#include "../../platform/input_owner.h"
#include "../../platform/menu_sound.h"
#include "../../platform/texture.h"
#include "../../platform/time.h"
#include "../game_state.h"
#include "chat_composer.h"
#include "chat_composer_model.h"
#include "chat_hud.h"
#include "menu_header.h"
#include "screen.h"
#include "widget_osk.h"

/* ---- layout ------------------------------------------------------------- */

struct chat_layout {
	struct menu_composition composition;
	struct menu_rect panel, header, input, keyboard;
	struct gutil_button_bounds send, cancel; /* bottom row pointer targets */
	struct gutil_hint_row hints;
	int hint_targets[5]; /* Send, Cancel, previous/next page, Backspace */
	struct chat_geometry g;					 /* the SPEC rect table (full) */
	int ox;
	int history_x, history_base, history_lines, history_width;
	bool compact;
};

static void chat_hint_row(struct chat_layout* l,
		const struct input_player_frame* owner, int width, int height) {
	const struct gutil_hint_item all[] = {
		{{IB_GUI_QUICK_MOVE}, 1, "Send", 3, false},
		{{IB_INVENTORY}, 1, "Cancel", 4, true},
		{{IB_GUI_PREVIOUS}, 1, "Prev", 2, false},
		{{IB_GUI_PAGE}, 1, "Next", 2, false},
		{{IB_GUI_CLICK_ALT}, 1, "Backspace", 1, false}};
	struct gutil_hint_item items[5];
	int tab = osk_chat_tab_of_page(gstate.players[0].chat.osk.page), count = 0;
	for(int k = 0; k < 5; k++) {
		if((k == 2 && (l->compact || tab == 0))
		   || (k == 3 && (l->compact || tab == OSK_CHAT_TABS - 1))) continue;
		items[count] = all[k];
		l->hint_targets[count++] = k < 2 ? k : k + 1;
	}
	(void)gutil_hint_row_layout(owner, 0, 0, width, height, l->compact,
		GUTIL_HINT_ALIGN_CENTRE, items, count, &l->hints);
	l->send = l->cancel = (struct gutil_button_bounds){0};
	for(int k = 0; k < l->hints.count; k++) {
		const struct gutil_hint_cell* cell = &l->hints.cells[k];
		int target = l->hint_targets[cell->item];
		if(target == 0) l->send = cell->rect;
		if(target == 1) l->cancel = cell->rect;
	}
}

static bool chat_row_key(const struct chat_layout* l, int target,
		struct gutil_button_bounds* key) {
	if(target < 3 || target > 5) return false;
	if(l->compact)
		return target == 5 && osk_sign_key_bounds(l->keyboard.x,
			l->keyboard.y, l->keyboard.w, true, 4, 1, key);
	if(target == 5)
		return osk_chat_key_bounds(l->ox, OSK_CHAT_ROW_UTILITY, OSK_CHAT_UTIL_BACK, key);
	int tab = osk_chat_tab_of_page(gstate.players[0].chat.osk.page)
		+ (target == 3 ? -1 : 1);
	return osk_chat_key_bounds(l->ox, OSK_CHAT_ROW_TABS, tab, key);
}

/* FULL layout: Astra's binding chat-keyboard design
 * (docs/ui_design/chat-keyboard/SPEC.md). Every rect comes from chat_geometry()
 * -- the SPEC rect table -- so the panel now spans y120..432 and the last key
 * row ends at y420, 36 px clear of the 456 title-safe bottom. The composer used
 * to sit at (44,236,552,236): bottom edge y=472 with the Space/Backspace row at
 * y440..468, i.e. inside the ~5% SDTV overscan band. That is the defect this
 * replaces.
 *
 * The 10-31 controller redesign puts the glyph hints below this panel. The
 * unreachable remote-chat compact path keeps its sign-profile key sizes and
 * moves the keyboard up to end at the split hint strip's y=196 edge. */
static void chat_layout(struct chat_layout* l, const struct input_player_frame* owner,
						int width, int height) {
	menu_composition_begin(&l->composition, MENU_SCREEN_CHAT, 0, 0, width, height);
	bool full = l->composition.layout == MENU_SCREEN_FULL;
	l->compact = !full;
	int ox = (width - 640) / 2, oy = (height - (full ? 480 : 240)) / 2;
	l->ox = ox;
	chat_geometry(&l->g, ox);
	if(full) {
		l->panel = (struct menu_rect) {l->g.panel.x, l->g.panel.y, l->g.panel.w,
									   l->g.panel.h};
		l->header = (struct menu_rect) {l->g.header.x, l->g.header.y,
										l->g.header.w, l->g.header.h};
		l->input = (struct menu_rect) {l->g.input.x, l->g.input.y, l->g.input.w,
									   l->g.input.h};
		l->keyboard = (struct menu_rect) {l->g.keyboard.x, l->g.keyboard.y,
										  l->g.keyboard.w, l->g.keyboard.h};
	} else {
		l->panel = (struct menu_rect) {ox + 32, oy + 8, 576, 188};
		l->header = (struct menu_rect) {l->panel.x + 12, l->panel.y + 8,
										l->panel.w - 24, 16};
		l->input = (struct menu_rect) {l->panel.x + 12, l->panel.y + 28,
									   l->panel.w - 24, 24};
		l->keyboard = (struct menu_rect) {l->panel.x + 12, l->panel.y + 55,
										  l->panel.w - 24, 133};
	}
	menu_composition_header(&l->composition, l->header.x, l->header.y, l->header.w,
							l->header.h);
	chat_hint_row(l, owner, width, height);
	/* The strip is outside the panel; the last full-size key ends at 420. */
	if(l->hints.count && l->panel.y + l->panel.h > l->hints.strip.y)
		l->panel.h = l->hints.strip.y - l->panel.y;
	menu_composition_window(&l->composition, l->panel.x, l->panel.y, l->panel.w,
							l->panel.h);
	if(full) {
		/* "Open history: newest text at (44,98) ... maximum 3 ... Clip both
		 * text and strips to (44,60,552,54)." */
		l->history_x = l->g.history.x;
		l->history_base = l->g.history_newest_y;
		l->history_lines = CHAT_HISTORY_OPEN_LINES;
		l->history_width = l->g.history.w;
	} else {
		l->history_x = ox + 2 * GFX_GUI_SCALE;
		l->history_base = l->panel.y - 10 * GFX_GUI_SCALE;
		l->history_lines = 0;
		l->history_width = 320 * GFX_GUI_SCALE;
	}
	menu_composition_action(&l->composition, l->input.x, l->input.y, l->input.w,
							l->input.h);
	menu_composition_action(&l->composition, l->keyboard.x, l->keyboard.y,
							l->keyboard.w, l->keyboard.h);
	if(full) {
		/* Include the row's Send/Cancel targets in notice conflict checks. */
		menu_composition_action(&l->composition, l->send.x, l->send.y,
								l->send.width, l->send.height);
		menu_composition_action(&l->composition, l->cancel.x, l->cancel.y,
								l->cancel.width, l->cancel.height);
	}
	/* Ledger row "Header title16,y+4 -> title20,y+4; composition measurement20
	 * too": the header's reservation must measure the title at the size it is
	 * actually drawn, or the notice could be reserved over it. */
	menu_composition_header_notice(&l->composition,
								   gutil_connection_notice_width(0, false, true),
								   gutil_font_width("Chat", full ? CHAT_FONT_KEY
																 : CHAT_FONT_LABEL));
	menu_composition_seal(&l->composition);
	if(full) {
		/* Chat reserves its OWN notice band above the panel (SPEC: "Reserved
		 * controller notice 44,24,552,28 ... Reserved notice band stays
		 * separate at y24..52; panel starts y120, history ends y114") instead
		 * of the shared (24,8,592,28) mount. The composition carries the rect
		 * that is actually drawn, so menu_composition_notice_conflict still
		 * proves it covers no declared control. Compact keeps the shared
		 * behaviour untouched. */
		l->composition.notice_in_header = false;
		l->composition.notice = (struct menu_rect) {
			l->g.notice.x, l->g.notice.y, l->g.notice.w, l->g.notice.h};
	}
}

/* ---- state -------------------------------------------------------------- */

static struct chat_composer* chat_state(void) {
	return &gstate.players[0].chat;
}

/* The ONE store behind screen_chat_observe (chat_composer.h). Written exactly
 * once per screen_chat_update, read-only for every caller; production reads
 * nothing back out of it, so its presence cannot change a frame. */
static struct screen_chat_observation chat_obs;

static void chat_close(struct chat_composer* c, enum menu_sound_event cue) {
	c->closed = true;
	audio_menu_event(cue, time_get());
}

/* ---- the composer's ONLY enqueue point ----------------------------------
 *
 * In a shipping binary this is exactly `svin_rpc_send(rpc)` -- no static, no
 * branch, no entry point. Under CAVEWII_TESTING (the project's explicit
 * test-instrumentation switch, the same one settle_observer_test_blocking_stop
 * lives behind) it gains the runtime-armed shape of the
 * displaylist_test_fail_* seams: a test can make the next N enqueues refuse
 * WITHOUT touching the transport -- i.e. a refused enqueue while the remote
 * session is still alive, which is the one SPEC state a PC rig cannot stage
 * (taking the server away ends the session and screen_chat_update closes the
 * composer first) -- and it counts the calls that really reached
 * svin_rpc_send, so "exactly one submission" is a number and not a claim.
 *
 * Kept out of every shipping binary BY THE BUILD, not by convention: only the
 * cavex_test_* pools carry CAVEWII_TESTING, CMakeLists' playable/production
 * boundary sweep FATAL_ERRORs on a playable target that carries the define,
 * and the post-link marker gates (cmake/check_no_test_marker.cmake for
 * cavewii_v2.exe, the Makefile's readelf grep for the .dol) reject a playable
 * artifact built from test objects. This is a TEST seam, never a production
 * failure lever: nothing outside tests/ can arm it. */
#ifdef CAVEWII_TESTING
static unsigned chat_refuse_remaining;
static unsigned chat_enqueue_accepted;

void screen_chat_test_refuse_sends(unsigned count) {
	chat_refuse_remaining = count;
}

unsigned screen_chat_test_enqueue_count(void) {
	return chat_enqueue_accepted;
}

void screen_chat_test_enqueue_reset(void) {
	chat_refuse_remaining = 0;
	chat_enqueue_accepted = 0;
}
#endif

static bool chat_enqueue(struct server_rpc* rpc) {
#ifdef CAVEWII_TESTING
	if(chat_refuse_remaining) {
		chat_refuse_remaining--;
		return false;
	}
#endif
	bool sent = svin_rpc_send(rpc);
#ifdef CAVEWII_TESTING
	if(sent)
		chat_enqueue_accepted++;
#endif
	return sent;
}

/* Enter [740c583 GuiChat.keyTyped key 28]: trim, send if non-empty, close.
 * The message crosses the provider boundary as SRPC_CHAT_SEND (the remote
 * provider sanitizes + encodes Packet3; server_local ignores it -- Beta SP's
 * send is a no-op). A refused send (pending queue full) keeps the draft and
 * the screen so nothing typed is lost silently. */
static void chat_send(struct chat_composer* c) {
	struct server_rpc rpc = {.type = SRPC_CHAT_SEND};
	size_t len = chat_composer_submit(c->buf, rpc.payload.chat_send.text,
									  sizeof(rpc.payload.chat_send.text));
	if(len == 0) {
		chat_close(c, MENU_SOUND_BACK);
		return;
	}
	if(!chat_enqueue(&rpc)) {
		/* SPEC "Error/refused enqueue": retain draft and focus, show `Retry`
		 * in the counter's own 88-px allocation and outline the input for
		 * 1200 ms. No discard, no success cue, no claimed acknowledgement. */
		c->retry_at = time_get();
		c->retry_armed = true;
		return;
	}
	chat_close(c, MENU_SOUND_SELECT);
}

/* ESC / Cancel [740c583 GuiChat.keyTyped key 1]: instant discard, no prompt
 * (the documented difference from the sign editor's discard confirmation). */
static void chat_cancel(struct chat_composer* c) {
	chat_close(c, MENU_SOUND_BACK);
}

static void screen_chat_reset(struct screen* screen, int width, int height) {
	(void)width;
	(void)height;
	struct chat_composer* c = chat_state();
	input_pointer_enable(true);
	/* B1b: with the agent bridge armed AND a menu button claimed, the composer
	 * stays OUT of PC text mode so the controller aliases (directions, page,
	 * click, backspace, send, cancel) are live through the production
	 * owner-frame path, exactly as on a Wii. Sampled once, here, so the mode
	 * cannot flip mid-session and duplicate a character between the text queue
	 * and the alias path. */
	/* Read through the core-owned flag, never by calling the app-only bridge:
	 * this file compiles into cavex_prod_core, which is NOT built with
	 * CAVEWII_AGENT_BRIDGE, so a direct call compiled out of the shipped app
	 * and the mode silently never existed (chat_composer_model.h explains the
	 * inversion). Sampled ONCE, here, so the mode cannot flip mid-session and
	 * duplicate a character between the text queue and the alias path. */
	c->controller_mode = chat_controller_mode_requested();
	input_text_enable(!c->controller_mode);
	/* The keystroke that opened this screen must not become text: drop any
	 * character queued before this frame (PC; the Wii queue is a stub). */
	char drop;
	while(input_text_poll(&drop)) {
	}
	screen->render3D = screen_ingame.render3D;
	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;
	c->buf[0] = 0; /* GuiChat(): message = "" */
	c->closed = false;
	c->update_counter = 0;
	c->tick_accum = 0.0F;
	c->pointer_available = false;
	c->focus = (struct gutil_menu_focus) {.initialized = true};
	c->menu_target = 2; /* the keyboard's initial q key */
	c->menu_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	osk_sign_reset(&c->osk, c->buf, sizeof(c->buf), CHAT_COMPOSER_CAP);
	/* Commands and chat are lowercase-first ("/register ..."), unlike a sign
	 * title, so open on the lower-case page. SPEC: "Start on lowercase q,
	 * never Send" -- row 1 col 0 of the Letters page IS 'q'. */
	c->osk.page = OSK_PAGE_LOWER;
	c->osk.row = OSK_CHAT_GRID_ROW_FIRST;
	c->osk.col = 0;
	c->kb = (struct osk_chat_view) {.ox = 0, .upper = false};
	/* B1b hygiene: every chat button that is down right now -- above all the
	 * one that just opened the composer -- is blocked until it is released, so
	 * the opening press cannot type, activate a key or send. */
	osk_chat_input_reset(&c->kb);
	c->press_row = c->press_col = -1;
	c->press_armed = c->cap_flash_armed = c->retry_armed = false;
	/* Decision item 4: the first-use hint retires once the composer opens. */
	gstate.chat_hint_done = true;
	/* No frame has been observed yet: screen_chat_observe stays false until
	 * the first update stores one (chat_composer.h). */
	chat_obs = (struct screen_chat_observation) {0};
}

bool screen_chat_observe(struct screen_chat_observation* out) {
	if(!out)
		return false;
	if(!chat_obs.open) {
		*out = (struct screen_chat_observation) {0};
		return false;
	}
	*out = chat_obs;
	return true;
}

static void chat_observe_store(const struct chat_composer* c,
							   const struct input_player_frame* owner,
							   const struct osk_chat_result* kb, bool open) {
	chat_obs = (struct screen_chat_observation) {
		.open = open,
		.owner_connected = owner && owner->connected,
		.moved = kb && kb->moved,
		.moved_repeat = kb && kb->moved_repeat,
		.page_changed = kb && kb->page_changed,
		.activated = kb && kb->activated,
		.rejected = kb && kb->rejected,
		.row = c->osk.row,
		.col = c->osk.col,
		.page = c->osk.page,
		.upper = c->kb.upper,
		.draft_len = (unsigned)strlen(c->buf),
		.rearm_mask = c->kb.rearm_mask,
		.repeat_remaining_s = c->kb.repeat.remaining,
		.repeat_seen_s = kb ? kb->repeat_seen_s : c->kb.repeat.remaining,
		.draft = c->buf};
}

static void screen_chat_update(struct screen* screen, float dt) {
	(void)screen;
	struct chat_composer* c = chat_state();
	if(c->closed || !gstate.remote_session) {
		/* Closed by Send/Cancel, or the session ended underneath us. */
		chat_obs.open = false;
		input_text_enable(false);
		screen_set(&screen_ingame);
		return;
	}
	/* GuiChat.updateScreen: updateCounter++ per 20 Hz client tick. */
	c->tick_accum += dt;
	while(c->tick_accum >= 0.05F) {
		c->tick_accum -= 0.05F;
		c->update_counter++;
	}
	struct input_player_frame owner;
	input_prompt_frame(0, &owner);
	struct chat_layout l;
	chat_layout(&l, &owner, gfx_width(), gfx_height());
	/* B1b owner isolation: the composer runs off PLAYER 0's own frame. A
	 * disconnected pad cannot type, and in a split session a second player's
	 * buttons never reach this composer. Where no device owns player 0 (PC
	 * without a paired controller) the merged mask is presented as that frame,
	 * so the behaviour there is exactly what it was. */
	struct input_player_frame merged;
	const struct input_player_frame* owner_in = input_player_frame(0);
	if(!owner_in) {
		/* PC with no paired controller: present the merged mask AS player 0's
		 * frame so the composer has exactly one input path and the widget
		 * never reaches for a global. Behaviour is what it always was there. */
		merged = (struct input_player_frame) {.connected = true};
		for(int b = 0; b < IB_COUNT; b++) {
			merged.pressed[b] = input_pressed((enum input_button)b);
			merged.down[b] = input_held((enum input_button)b);
		}
		owner_in = &merged;
	}
	/* Advance the neutral re-arm before ANY edge is read this frame. */
	osk_chat_input_tick(&c->kb, owner_in);
	bool typing = !c->controller_mode && input_text_active();
	/* "Static pointer must not reclaim directional focus" -- a directional
	 * press from the OWNER keeps the cursor with the d-pad. */
	bool nav = !typing && owner_in->connected
		&& (owner_in->pressed[IB_GUI_LEFT] || owner_in->pressed[IB_GUI_RIGHT]
			|| owner_in->pressed[IB_GUI_UP] || owner_in->pressed[IB_GUI_DOWN]);
	c->pointer_available
		= input_pointer(&c->pointer_x, &c->pointer_y, &c->pointer_angle);
	bool pointer = gutil_menu_pointer_focus(&c->focus, c->pointer_available,
											c->pointer_x, c->pointer_y, nav);
	if(nav) {
		c->menu_target = 2;
		c->menu_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	}
	/* B keeps its text-editor meaning (backspace) and outranks A in a
	 * combined report -- the widget performs the deletion itself. Read from
	 * the owner's frame like every other chat edge. */
	bool backspace = osk_chat_back_edge(&c->kb, owner_in);
	bool click = osk_chat_click_edge(&c->kb, owner_in) && !backspace;
	bool send = false, cancel = false;
	if(!typing) {
		/* A focused Beta text screen owns the whole keyboard: while a PC
		 * keyboard types, IB_* aliases of printable keys stay dark
		 * (input.h's input_text_active contract), so T (quick-move/Send)
		 * never submits mid-word -- Enter does. Both edges go through the
		 * composer's neutral re-arm, so the press that opened the screen
		 * cannot submit and a held Send cannot fire twice. */
		send = osk_chat_send_edge(&c->kb, owner_in);
		cancel = osk_chat_cancel_edge(&c->kb, owner_in);
	}
	int hovered = GUTIL_MENU_TARGET_NONE;
	if(c->pointer_available) {
		int item = gutil_hint_row_hit(&l.hints, c->pointer_x, c->pointer_y);
		if(item >= 0) hovered = l.hint_targets[item];
		else {
			struct gutil_button_bounds keys = {l.keyboard.x,l.keyboard.y,
				l.keyboard.w,l.keyboard.h};
			if(gutil_button_hit(&keys,c->pointer_x,c->pointer_y)) hovered = 2;
		}
	}
	/* Over blank the pointer keeps the last REAL target (Fable G4 review 1):
	 * pointing the Remote away then gives A back to it under rule (3) -- the
	 * keyboard types -- as in screen_pause.c / container_menu.c. */
	if(pointer) {
		if(hovered != GUTIL_MENU_TARGET_NONE) c->menu_target = hovered;
		c->menu_origin = GUTIL_MENU_ORIGIN_POINTER;
	}
	bool selected_enabled = c->menu_target == 2;
	for(int k = 0; k < l.hints.count; k++)
		selected_enabled |= l.hint_targets[l.hints.cells[k].item] == c->menu_target;
	struct gutil_menu_decision decision = gutil_menu_activate_decide(
		(struct gutil_menu_activate_state) {
			.pressed = click, .navigated = nav,
			.pointer_valid = c->pointer_available,
			.selection_origin = c->menu_origin, .hovered = hovered,
			.hovered_enabled = hovered >= 0, .selected = c->menu_target,
			.selected_enabled = selected_enabled,
		});
	c->menu_target = decision.selected;
	c->menu_origin = decision.selection_origin;
	bool psend = decision.action == GUTIL_MENU_ACTIVATE && decision.target == 0;
	bool pcancel = decision.action == GUTIL_MENU_ACTIVATE && decision.target == 1;
	bool row_key = decision.action == GUTIL_MENU_ACTIVATE && decision.target >= 3;
	/* Every click on a row target is consumed, including focus-only and
	 * same-frame navigation. The keyboard receives A only for its own target. */
	bool control = click && (decision.action != GUTIL_MENU_ACTIVATE
		|| decision.target != 2);
	/* Route page/Backspace row clicks through the keyboard's existing key
	 * activation. The real owner A edge still supplies re-arm and text-queue
	 * precedence; no second edit or paging path is introduced. */
	float key_x = c->pointer_x, key_y = c->pointer_y;
	bool key_pointer = pointer;
	if(row_key) {
		struct gutil_button_bounds key;
		row_key = chat_row_key(&l, decision.target, &key);
		if(row_key) {
			key_x = key.x + key.width / 2;
			key_y = key.y + key.height / 2;
			key_pointer = true;
		}
	}
	char before[CHAT_COMPOSER_CAP + 1];
	memcpy(before, c->buf, sizeof(before));
	int page_before = c->osk.page;
	bool queued = false, focus_moved = false;
	enum osk_event ev;
	/* Hoisted out of the else-branch (was a local there) so the end-of-frame
	 * observation store can read the SAME result the screen just acted on.
	 * osk_chat_update_owned overwrites *out wholesale, so this initialiser is
	 * only what the compact path -- which never fills it -- leaves behind. */
	struct osk_chat_result kb = {.press_row = -1, .press_col = -1};
	if(l.compact) {
		/* Compact uses the existing sign-profile key mutator. */
		ev = osk_sign_update_owned(&c->osk, l.keyboard.x, l.keyboard.y,
								   l.keyboard.w, true, key_pointer, key_x,
								   key_y, !send && !cancel && (!control || row_key),
								   &queued, owner_in, !c->controller_mode);
	} else {
		c->kb.ox = l.ox;
		osk_chat_update_owned(&c->osk, &c->kb, dt, key_pointer, key_x,
							  key_y, !send && !cancel && (!control || row_key),
							  owner_in, !c->controller_mode, &kb);
		ev = kb.event;
		queued = kb.text_queued;
		focus_moved = kb.moved && !kb.moved_repeat;
		if(kb.activated) {
			/* SPEC "Pressed": label down 1 px for 80 ms; the hit rect and the
			 * layout never move. */
			c->press_row = kb.press_row;
			c->press_col = kb.press_col;
			c->press_at = time_get();
			c->press_armed = true;
		}
		if(kb.rejected) {
			/* "At cap, text-producing keys (including Space) reject insertion
			 * with unchanged buffer and flash the counter yellow for 120 ms." */
			c->cap_flash_at = time_get();
			c->cap_flash_armed = true;
		}
	}
	/* SPEC "Motion": one highest-priority cue per update -- Type and Section
	 * outrank Focus, and FOCUS fires only on the first actual move of a fresh
	 * directional press or a new pointer target, never on a held-repeat step
	 * and never when the cursor could not move. */
	if(strcmp(before, c->buf) != 0)
		audio_menu_event(MENU_SOUND_TYPE, time_get());
	else if(c->osk.page != page_before)
		audio_menu_event(MENU_SOUND_SECTION, time_get());
	else if(focus_moved)
		audio_menu_event(MENU_SOUND_FOCUS, time_get());
	/* The ONE observation store (chat_composer.h). Taken after production has
	 * decided everything this frame and before the close dispatch, so a
	 * scripted run reads exactly what the screen acted on. */
	chat_observe_store(c, owner_in, l.compact ? NULL : &kb, true);
	if(!queued) {
		send |= psend;
		cancel |= pcancel;
	}
	if(cancel || ev == OSK_EV_CANCEL) {
		chat_cancel(c);
		chat_obs.open = false;
		return;
	}
	if(send || ev == OSK_EV_DONE) {
		chat_send(c);
		/* A REFUSED enqueue retains the draft AND the screen, so the composer
		 * is still open -- only a real close clears the observation. */
		chat_obs.open = !c->closed;
	}
}

/* ---- rendering ---------------------------------------------------------- */

static void chat_rect(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b,
					  uint8_t a) {
	/* History rendering restores MODE_OFF. Each translucent rectangle must
	 * establish its own blend state, including the draft drawn after history. */
	gfx_blending(MODE_BLEND);
	gfx_alpha_test(false);
	gfx_texture(false);
	gutil_texquad_col(x, y, 0, 0, 0, 0, w, h, r, g, b, a);
	gfx_texture(true);
	gfx_blending(MODE_OFF);
	gfx_alpha_test(true);
}

/* A 2-px outline on a rect's own edges, in the given ink (the refused-enqueue
 * input outline, SPEC's error state). */
static void chat_outline(int x, int y, int w, int h, uint8_t r, uint8_t g,
						 uint8_t b) {
	const int e = 2;
	gfx_texture(false);
	gutil_texquad_col(x, y, 0, 0, 0, 0, w, e, r, g, b, 255);
	gutil_texquad_col(x, y + h - e, 0, 0, 0, 0, w, e, r, g, b, 255);
	gutil_texquad_col(x, y + e, 0, 0, 0, 0, e, h - 2 * e, r, g, b, 255);
	gutil_texquad_col(x + w - e, y + e, 0, 0, 0, 0, e, h - 2 * e, r, g, b, 255);
	gfx_texture(true);
}

/* One chat key in the SPEC's state table ("Art, typography and states"):
 *   default  normal widget row (y66..86), #E0E0E0 label with shadow
 *   hover    selected row (y86..106) -- the one focus target the pointer picks
 *   focus    selected row + an immediate white 2-px OUTER border, no movement
 *   pressed  normal row + a 2-px dark INSET border, label down 1 px for 80 ms,
 *            hit rect unchanged
 *   disabled disabled row (y46..66) and an #A0A0A0 label, no focus/activation
 * The split-half stretch and the sheet rows are the existing ones; no new
 * raster asset and no invented chrome. */
static void chat_key_draw(const struct gutil_button_bounds* q, const char* label,
						  int font, bool selected, bool pressed,
						  bool disabled, bool focused) {
	int sy = disabled ? OSK_CHAT_TEX_DISABLED
					  : selected ? OSK_CHAT_TEX_SELECTED : OSK_CHAT_TEX_NORMAL;
	int half = q->width / 2;
	gfx_bind_texture(&texture_gui_widgets);
	gutil_texquad(q->x, q->y, 0, sy, 100, 20, half, q->height);
	gutil_texquad(q->x + half, q->y, 100, sy, 100, 20, q->width - half,
				  q->height);
	if(focused)
		gutil_menu_focus_border(q);
	if(pressed) {
		const int e = OSK_CHAT_BORDER;
		chat_outline(q->x + e, q->y + e, q->width - 2 * e, q->height - 2 * e,
					 0x40, 0x40, 0x40);
	}
	int lx = q->x + (q->width - gutil_font_width(label, font)) / 2;
	int ly = q->y + (q->height - font) / 2 + (pressed ? 1 : 0);
	uint8_t ink = disabled ? 0xA0 : 0xE0;
	uint8_t blue = disabled ? 0xA0 : 0xE0;
	if(!disabled && selected)
		ink = 0xFF, blue = 0xE0; /* focus-yellow #FFFFE0 */
	gutil_text_rgb(lx, ly, label, font, true, ink, ink, blue);
}

/* The chat keyboard: three named tabs, the character grid and the utility row,
 * over the chat-scoped geometry in widget_osk.c. `pressed_row`/`pressed_col` is
 * the key still inside its 80 ms pressed window, or -1/-1. */
static void chat_keyboard_render(const struct osk_state* o,
								 const struct osk_chat_view* view,
								 int pressed_row, int pressed_col) {
	int ox = view->ox;
	int tab = osk_chat_tab_of_page(o->page);
	for(int row = 0; row < OSK_CHAT_ROWS; row++) {
		int n = osk_chat_row_count(o->page, row);
		for(int col = 0; col < n; col++) {
			struct gutil_button_bounds q;
			if(!osk_chat_key_bounds(ox, row, col, &q))
				continue;
			char one[2] = {0};
			const char* label = one;
			bool disabled = false;
			if(row == OSK_CHAT_ROW_TABS) {
				label = osk_chat_tab_label(col);
			} else if(row == OSK_CHAT_ROW_UTILITY) {
				if(col == OSK_CHAT_UTIL_CASE) {
					label = osk_chat_case_label(view->upper);
					disabled = !osk_chat_case_enabled(o->page);
				} else if(col == OSK_CHAT_UTIL_SPACE) {
					/* "Space is a plain labelled key activated by A." */
					label = "Space";
				} else {
					label = "Backspace";
				}
			} else {
				one[0] = osk_sign_page_char(o->page, row, col);
				if(!one[0])
					continue;
			}
			bool focused = !disabled && o->row == row && o->col == col;
			/* The active tab keeps the selected texture even without focus. */
			bool selected = focused || (row == OSK_CHAT_ROW_TABS && col == tab);
			bool pressed = row == pressed_row && col == pressed_col;
			bool is_char = row >= OSK_CHAT_GRID_ROW_FIRST
				&& row <= OSK_CHAT_GRID_ROW_LAST;
			chat_key_draw(&q, label, is_char ? CHAT_FONT_KEY : CHAT_FONT_LABEL,
						  selected, pressed, disabled, focused);
			if(row == OSK_CHAT_ROW_TABS && col == tab) {
				gfx_texture(false);
				gutil_texquad_col(q.x + OSK_CHAT_TAB_UNDERLINE_INSET,
								  OSK_CHAT_TAB_UNDERLINE_Y, 0, 0, 0, 0,
								  q.width - 2 * OSK_CHAT_TAB_UNDERLINE_INSET,
								  OSK_CHAT_TAB_UNDERLINE_H, 0xFF, 0xFF, 0xFF,
								  255);
				gfx_texture(true);
			}
		}
	}

}

static void screen_chat_render2D(struct screen* screen, int width, int height) {
	(void)screen;
	struct chat_composer* c = chat_state();
	struct input_player_frame owner;
	input_prompt_frame(0, &owner);
	struct chat_layout l;
	chat_layout(&l, &owner, width, height);
	/* Dim the world a little less than the sign editor does: the history
	 * lines above the panel are read against it. SPEC "World remains live":
	 * the world keeps rendering, nothing is blurred or paused. */
	chat_rect(0, 0, width, height, 0, 0, 0, 140);
	/* GuiIngame's chat block in its "GuiChat open" branch: every line opaque
	 * [740c583 GuiIngame.renderGameOverlay]. */
	if(l.compact)
		chat_hud_render(&gstate.chat_ring, l.history_x, l.history_base, true,
						l.history_lines);
	else
		chat_hud_render_open(&gstate.chat_ring, l.history_x, l.history_base,
							 l.history_lines, l.history_width);
	screen_menu_panel_draw(&l.panel);
	if(l.compact) {
		gutil_text_rgb(l.header.x, l.header.y + 4, "Chat", 16, false, 64, 64, 64);
		chat_rect(l.input.x, l.input.y, l.input.w, l.input.h, 0, 0, 0, 128);
		const char* cursor = (c->update_counter / 6) % 2 == 0 ? "_" : "";
		const char* tail = c->buf;
		char line[CHAT_COMPOSER_CAP + 8];
		int text_w = l.input.w - 8;
		for(;;) {
			snprintf(line, sizeof(line), "> %s%s", tail, cursor);
			if(gutil_font_width(line, 16) <= text_w || !*tail)
				break;
			tail++; /* deviation (2): show the tail of an overlong draft */
		}
		gutil_text_rgb(l.input.x + 4, l.input.y + (l.input.h - 16) / 2, line, 16,
					   true, 0xE0, 0xE0, 0xE0);
		osk_sign_render(&c->osk, l.keyboard.x, l.keyboard.y, l.keyboard.w, true);
	} else {
		ptime_t now = time_get();
		bool retry = c->retry_armed
			&& time_diff_ms(c->retry_at, now) < CHAT_RETRY_MS;
		/* Header title; action hints are in the bottom row. */
		gutil_text_rgb(l.g.title_x, l.g.title_y, "Chat", CHAT_FONT_KEY, false,
					   0x40, 0x40, 0x40);
		/* GuiChat.drawScreen: drawRect(2, h-14, w-2, h-2, 0x80000000) and
		 * "> " + message + a "_" cursor shown while (updateCounter / 6) % 2 ==
		 * 0, colour 0xe0e0e0. */
		chat_rect(l.input.x, l.input.y, l.input.w, l.input.h, 0, 0, 0, 128);
		if(retry)
			chat_outline(l.input.x, l.input.y, l.input.w, l.input.h, 0xFF, 0xFF,
						 0xE0);
		const char* cursor = (c->update_counter / 6) % 2 == 0 ? "_" : "";
		const char* tail = c->buf;
		char line[CHAT_COMPOSER_CAP + 8];
		/* "fixed 416-pixel draft clip ending 480; suffix scroll includes
		 * underscore budget" -- the caret's width is always reserved, so the
		 * tail does not jitter as the caret blinks. */
		for(;;) {
			snprintf(line, sizeof(line), "> %s_", tail);
			if(gutil_font_width(line, CHAT_FONT_LABEL) <= l.g.draft.w || !*tail)
				break;
			tail++; /* deviation (2): show the tail of an overlong draft */
		}
		snprintf(line, sizeof(line), "> %s%s", tail, cursor);
		gutil_text_rgb(l.g.draft.x, l.g.draft.y, line, CHAT_FONT_LABEL, true,
					   0xE0, 0xE0, 0xE0);
		/* "counter right 576, y172, font16, width 88, n/100 always". At the cap
		 * (or on a refused byte) it is focus-yellow; a refused enqueue replaces
		 * it with `Retry` inside the same 88-px allocation. */
		char count[16];
		size_t len = strlen(c->buf);
		snprintf(count, sizeof(count), "%u/%u", (unsigned)len,
				 (unsigned)CHAT_COMPOSER_CAP);
		bool flash = c->cap_flash_armed
			&& time_diff_ms(c->cap_flash_at, now) < CHAT_CAP_FLASH_MS;
		bool yellow = !retry && ((int)len >= CHAT_COMPOSER_CAP || flash);
		const char* counter_text = retry ? "Retry" : count;
		gutil_text_rgb(l.g.counter.x + l.g.counter.w
						   - gutil_font_width(counter_text, CHAT_FONT_LABEL),
					   l.g.counter.y, counter_text, CHAT_FONT_LABEL, true,
					   yellow ? 0xFF : 0xE0, yellow ? 0xFF : 0xE0, 0xE0);
		int press_row = -1, press_col = -1;
		if(c->press_armed && time_diff_ms(c->press_at, now) < CHAT_PRESS_MS)
			press_row = c->press_row, press_col = c->press_col;
		/* Render may run before the first update of a frame, so take the
		 * offset from the layout that was just measured. */
		struct osk_chat_view view = c->kb;
		view.ox = l.ox;
		chat_keyboard_render(&c->osk, &view, press_row, press_col);
	}
	gutil_hint_row_draw(&l.hints);
	for(int k = 0; k < l.hints.count; k++) {
		const struct gutil_hint_cell* cell = &l.hints.cells[k];
		if(c->menu_target == l.hint_targets[cell->item])
			gutil_menu_focus_border(&cell->rect);
	}
	screen_notice_compose(l.composition.notice.x, l.composition.notice.y,
						  l.composition.notice.w, l.composition.notice.h,
						  l.composition.notice_in_header);
	if(c->pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(c->pointer_x, c->pointer_y, glm_rad(c->pointer_angle),
							 0, 0, 256, 256, 48, 48);
	}
	/* The hint row leaves MODE_BLEND set; this screen's contract (pinned by
	 * test_ui_geometry) is to hand the frame back with blending off. */
	gfx_blending(MODE_OFF);
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* Row cells and the keyboard keys they activate, without a graphics context. */
void screen_chat_test_row(const struct input_player_frame* owner, int width,
		int height, struct gutil_hint_row* row, int targets[5],
		struct gutil_button_bounds keys[5]) {
	struct chat_layout l;
	chat_layout(&l, owner, width, height);
	*row = l.hints;
	for(int k = 0; k < row->count; k++) {
		targets[k] = l.hint_targets[row->cells[k].item];
		keys[k] = (struct gutil_button_bounds){0};
		(void)chat_row_key(&l, targets[k], &keys[k]);
	}
}

void screen_chat_test_layout(int width, int height, struct menu_composition* out,
							 struct menu_rect* input, struct menu_rect* keyboard,
							 int* history_lines) {
	struct input_player_frame owner;
	input_prompt_frame(0, &owner);
	struct chat_layout l;
	chat_layout(&l, &owner, width, height);
	if(out)
		*out = l.composition;
	if(input)
		*input = l.input;
	if(keyboard)
		*keyboard = l.keyboard;
	if(history_lines)
		*history_lines = l.history_lines;
}

/* The bottom row's Send/Cancel targets and the unchanged full panel table.
 * The retired in-header Type hint reports an empty rectangle. */
void screen_chat_test_hints(int width, int height, struct chat_geometry* g,
							struct gutil_button_bounds* send,
							struct gutil_button_bounds* cancel,
							struct gutil_button_bounds* type_hint,
							bool* type_shown) {
	struct input_player_frame owner;
	input_prompt_frame(0, &owner);
	struct chat_layout l;
	chat_layout(&l, &owner, width, height);
	if(g)
		*g = l.g;
	if(send)
		*send = l.send;
	if(cancel)
		*cancel = l.cancel;
	if(type_hint)
		*type_hint = (struct gutil_button_bounds){0};
	if(type_shown)
		*type_shown = false;
}
#endif

struct screen screen_chat = {
	.reset = screen_chat_reset,
	.update = screen_chat_update,
	.render2D = screen_chat_render2D,
	.render3D = NULL,
	.render_world = true,
};
