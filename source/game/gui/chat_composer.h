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

#ifndef CHAT_COMPOSER_H
#define CHAT_COMPOSER_H

#include <stdbool.h>

#include "../../graphics/menu_focus.h"
#include "../../graphics/gui_util.h"
#include "../../platform/time.h"
#include "chat_composer_model.h"
#include "widget_osk.h"

/* Per-session state of the multiplayer chat/command composer screen
 * (screen_chat.c), owned by the primary client_session exactly like the
 * sign editor's `sign`. Only player 0 ever opens it: a remote session has one
 * local player and split-screen has no online mode (design record,
 * docs/mp_chat_auth_research_2026-09-11.md). */
struct chat_composer {
	struct osk_state osk;
	char buf[CHAT_COMPOSER_CAP + 1]; /* the GuiChat `message` draft */
	struct gutil_menu_focus focus;
	int menu_target; /* keyboard or one of the two pointer-only header actions */
	enum gutil_menu_origin menu_origin;
	bool pointer_available;
	float pointer_x, pointer_y, pointer_angle;
	bool closed;
	/* GuiChat.updateCounter: +1 per 20 Hz client tick, drives the "_" cursor
	 * blink ((counter / 6) % 2 == 0 shows it) [740c583 GuiChat.drawScreen]. */
	int update_counter;
	float tick_accum;
	/* Chat keyboard view: the raster offset and the case the Letters tab
	 * remembers (docs/ui_design/chat-keyboard/SPEC.md, "Keyboard grid and page
	 * states"). */
	struct osk_chat_view kb;
	/* SPEC's timed states. `press_*` is the key inside its 80 ms pressed
	 * window; `cap_flash_at` flashes the counter yellow for 120 ms when a byte
	 * is refused at the cap; `retry_at` shows "Retry" for 1200 ms after a
	 * refused enqueue. A zero `*_row` sentinel is not enough (row 0 is the tab
	 * row), so the -1 convention is used. */
	int press_row, press_col;
	ptime_t press_at, cap_flash_at, retry_at;
	bool press_armed, cap_flash_armed, retry_armed;
	/* B1b agent-armed controller mode (Astra 2026-09-17, approved scoped to
	 * chat and to explicit bridge activation). Sampled ONCE when the composer
	 * opens: with the CAVEWII_AGENT bridge armed AND a menu button claimed the
	 * screen stays out of PC keyboard text mode, so the controller aliases run
	 * through the production owner-frame path exactly as they do on a Wii.
	 * Unarmed, or armed but driving only gameplay, ordinary PC text entry is
	 * untouched. */
	bool controller_mode;
};

/* ---- read-only composer observation (mp_chat_login_3m, contract section 2)
 *
 * Focus, draft and the repeat/re-arm state are already public through
 * gstate.players[0].chat; the ONE thing that is not is the per-frame
 * `struct osk_chat_result`, a local in screen_chat_update. The scripted
 * mp_chat_login_3m driver (source/game/chat_login_script.c) has to assert
 * production's own answer -- "did the cursor move, and was that move a held
 * repeat" -- rather than re-derive it, so screen_chat_update stores its result
 * once at the end of the frame and this copies it out.
 *
 * READ-ONLY: no behaviour changes, nothing is consumed, and calling it (or
 * never calling it) cannot alter a single frame of the composer. `draft`
 * points at the live composer buffer and is valid until the next update.
 *
 * Returns FALSE when the composer is closed -- either it was never opened or
 * Send/Cancel closed it -- and then zeroes `*out`. The draft buffer itself is
 * not cleared until the composer next opens (production behaviour), so a
 * closed observation reports draft_len 0 BY DEFINITION, not by inspecting a
 * buffer that still holds the submitted text. */
struct screen_chat_observation {
	bool open, owner_connected, moved, moved_repeat, page_changed, activated,
		rejected;
	int row, col, page;
	bool upper;
	unsigned draft_len, rearm_mask;
	/* The countdown left AFTER the frame (post-reload), and the one the frame's
	 * step decision actually consulted (post-decrement, PRE-reload -- see
	 * struct osk_chat_result::repeat_seen_s). They differ on exactly the frames
	 * that matter: a stepping frame leaves 80 ms but decided on <= 0. */
	float repeat_remaining_s, repeat_seen_s;
	const char* draft;
};
bool screen_chat_observe(struct screen_chat_observation* out);

/* Test-only refused-enqueue seam (screen_chat.c). Arms the composer's single
 * enqueue point to refuse the next `count` sends without touching the
 * transport -- SPEC's "Error/refused enqueue" with the remote session still
 * alive -- and reports how many sends really reached svin_rpc_send.
 * Compiled ONLY under CAVEWII_TESTING: a shipping binary carries neither the
 * entry points nor the statics they flip, and the configure-time
 * playable-target sweep plus the post-link test-marker gates enforce that
 * (the settle_observer_test_blocking_stop precedent). */
#ifdef CAVEWII_TESTING
void screen_chat_test_refuse_sends(unsigned count);
unsigned screen_chat_test_enqueue_count(void);
void screen_chat_test_enqueue_reset(void);
#endif

#endif
