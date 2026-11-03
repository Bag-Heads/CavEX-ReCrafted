/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../config.h"
#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../platform/audio.h"
#include "../../platform/audio_music.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/time.h"
#include "../../network/client_interface.h"
#include "../../network/server_interface.h"
#include "../client_session.h"
#include "../difficulty.h"
#include "../game_state.h"
#include "screen.h"
#include "frontend_presentation.h"
#include "../../platform/input_owner.h"
#include "../../platform/input_glyph.h"

enum options_id {
	/* EnumOptions ordinals [740c583 EnumOptions]. */
	OPTIONS_MUSIC = 0,
	OPTIONS_SOUND = 1,
	OPTIONS_DIFFICULTY = 9, /* EnumOptions.DIFFICULTY ordinal (EnumOptions.java:13) */
	OPTIONS_VIDEO = 101,
	OPTIONS_CONTROLS = 102,
	OPTIONS_SPLIT = 150, /* experimental.local_splitscreen -- project extension, no Beta ordinal (Astra ui9, 2026-09-11) */
	OPTIONS_DONE = 200,
};

enum options_control_index {
	OC_MUSIC = 0,
	OC_SOUND,
	OC_VIDEO,
	OC_SPLIT,
	OC_DONE,
	/* Appended so the existing 0..4 control-index contract stays stable. */
	OC_CONTROLS,
	OC_DIFFICULTY, /* next-gaps-0924 W1F, appended for the same reason */
	OC_COUNT,
};

struct options_control {
	int id;
	const char* name;
	bool slider;
	bool enabled;
};

/* Native controller presentation (native_rework_2026-09-04.md): expose only
 * working settings. The approved Player 1 look preferences now have a real
 * input/config backend. Difficulty (next-gaps-0924 W1F) is Beta's own Options
 * row -- GuiOptions.java:7 lists EnumOptions.DIFFICULTY beside Music/Sound --
 * and drives the local server's world difficulty (source/game/difficulty.h). */
static const struct options_control controls[OC_COUNT] = {
	{OPTIONS_MUSIC, "Music", true, true},
	{OPTIONS_SOUND, "Sound", true, true},
	{OPTIONS_VIDEO, "Display...", false, true},
	{OPTIONS_SPLIT, "Local split-screen", false, true},
	{OPTIONS_DONE, "Back", false, true},
	{OPTIONS_CONTROLS, "Controller settings...", false, true},
	{OPTIONS_DIFFICULTY, "Difficulty", false, true},
};

#define OPTIONS_COUNT OC_COUNT

static struct screen* options_return = &screen_title;
/* ---- Live "Local split-screen" row (Astra option A, S-A2) ----------------
 * The row applies the switch WITHOUT a restart. server_local_set_split_enabled
 * is still boot-only on purpose (the server thread owns split_enabled, the
 * residency block and the transport accept class once it runs), so the live
 * route is a request/ack pair: SRPC_SPLIT_SET out, CRPC_SPLIT_STATE back,
 * applied on the server thread between ticks. Nothing here writes a server
 * field, and nothing arms input before the matching ack says ok.
 *
 * `split_value` is what the row DISPLAYS/asks for; `split_applied` is the last
 * value the SERVER acknowledged -- the only one input arming and the config
 * key may follow. On the title screen (no server) there is nothing to ask, so
 * the row keeps its original behaviour: the value is committed with the audio
 * keys and the boot path applies it at the next launch. */
enum split_row_state {
	SPLIT_ROW_SETTLED = 0, /* nothing outstanding; the row shows ON/OFF */
	SPLIT_ROW_PENDING,	   /* a request is waiting for its matching ack */
	SPLIT_ROW_FAILED,	   /* the last ack said ok=false; reason shown */
	SPLIT_ROW_NO_ANSWER,   /* the pending request was never answered */
};
/* A toggle the server never answers must NOT be read as success. Five seconds
 * is far past a between-ticks transition (the work is one mem2 block) and past
 * a saturated server-thread tick backlog, so a timeout here means the request
 * is genuinely lost -- the row then keeps the PREVIOUS state and says so. */
#define SPLIT_ACK_TIMEOUT_S 5.0F
/* Status-line budget, NUL included. The reason lands on the helper line
 * directly under the row -- the line that used to carry the restart claim --
 * because the 200 GUI-px row strip physically cannot hold a 48-char
 * CRPC_SPLIT_REASON_CAP string. 48 visible chars is the width the menu already
 * proves it can draw ("When active, second controller presses A to join." is
 * 49), and anything longer is truncated with "..." rather than silently cut. */
#define SPLIT_STATUS_CAP 49
static int split_value;
static bool split_applied;
/* False until main.c's boot path has told us what it applied. Before that the
 * config file is the only truth the row can show (there is no server yet). */
static bool split_applied_seeded;
static enum split_row_state split_row_state;
static uint32_t split_request_seq; /* monotonic per session; 0 = never sent */
static uint32_t split_pending_seq;
static bool split_pending_want;
static float split_pending_age;
static char split_status[SPLIT_STATUS_CAP];
/* The world generation (clin_input_epoch) this row has already re-seeded from
 * the config file -- 0 = none yet, which is also the value the boot seed
 * leaves behind because no handoff has happened before the main loop. Every
 * NEW generation gets exactly one seed attempt (R-A9 A9-M5); see
 * screen_options_split_world_seed. */
static uint32_t split_seed_generation;
/* R-A8 A8-M6: the acknowledged key is moved IN MEMORY on the ack and written
 * through by the existing Options transaction, never by a blocking config_save
 * from inside a gameplay frame (on the Wii that is a libfat SD write on the
 * render thread). True while the in-memory value has not been persisted yet.
 * Note config_save() serialises the WHOLE gstate.config_user object, so any
 * later commit -- Options, Display, Texture packs, Multiplayer setup -- also
 * flushes this key; options_commit() is only the nearest one. */
static bool split_key_dirty;
/* ---- Heartbeat evidence for the live toggle (Astra H3, S-A3) -------------
 * Astra's H3 question is "what did a live enable COST" -- the allocation
 * latency and the MEM2 headroom around it. The headroom half is already in
 * the stream (mem2_carve_free_kb, every second); the latency half is measured
 * here, because this is the only place that knows when the request left and
 * when its matching ack came back.
 *
 * The clock is time_get()/time_diff_ms rather than the accumulated `dt` the
 * timeout above ages with: `dt` is a frame delta the poll only receives when
 * something is pending and would report the round trip quantised to whole
 * frames (and 0 for an ack consumed on the same frame as the request). These
 * are two independent questions -- "has this been outstanding too long"
 * (frames, cheap) and "how long did the server actually take" (wall clock) --
 * so they use the instrument that answers each. Nothing here feeds back into
 * the state machine: a wrong reading can make a heartbeat field wrong, never
 * the toggle. */
static ptime_t split_request_sent;
static bool split_request_stamped; /* false = nothing to measure against */
static uint32_t split_toggle_ms;
static int split_last_ok = -1; /* -1 = nothing has settled yet */
static int selected = OC_MUSIC;
static struct gutil_menu_focus focus;
static struct gutil_menu_repeat repeat;
static int pointer_target = -1;
static bool pointer_available;
static float pointer_x, pointer_y, pointer_angle;
static enum gutil_menu_origin origin;
static float sound_value = 1.0F;
static float music_value = 1.0F;
/* W1F: the row's value -- GameSettings.difficulty. Seeded from the config on
 * every open; applied to the local server on every change (Beta's option is
 * live: Minecraft.java:1129 copies it into the world each tick) and written to
 * the config with the audio keys in options_commit (GuiOptions saves on
 * leave, GuiOptions.java:44-50). */
static int difficulty_value = DIFFICULTY_DEFAULT;
static bool save_failed;
static char labels[OPTIONS_COUNT][64];

static struct split_off_transaction {
	enum screen_options_split_off_phase phase;
	uint32_t world_generation;
	struct client_local_leave_token leave;
	uint32_t split_request_seq;
	int selected;
	struct gutil_menu_focus focus;
	enum gutil_menu_origin origin;
} split_off;
static bool split_off_rearm;

static bool split_off_rearm_step(bool active) {
	if(!split_off_rearm)
		return false;
	if(!active)
		split_off_rearm = false;
	return true;
}

bool screen_options_split_off_rearm_step(bool active) {
	return split_off_rearm_step(active);
}

void screen_options_set_return(struct screen* s) {
	options_return = s ? s : &screen_title;
}

bool screen_options_from_pause(void) {
	return options_return == &screen_pause;
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
/* GuiSlider pointer mapping: (mouseX-(x+4))/(width-8), clamp 0..1.
 * [740c583 GuiSlider.mousePressed]
 * [740c583 GuiSlider.mouseDragged] */
static float options_slider_value(float mouse_x, int x, int width) {
	float value = (mouse_x - (float)(x + 4)) / (float)(width - 8);
	if(value < 0.0F)
		value = 0.0F;
	if(value > 1.0F)
		value = 1.0F;
	return value;
}

/* GuiSlider handle x: x + (int)(value*(width-8)).
 * [740c583 GuiSlider.mouseDragged] */
static int options_slider_thumb(int x, int width, float value) {
	return x + (int)(value * (float)(width - 8));
}

#endif

/* Keep Beta's Music/Sound label composition and live independent values. */
static void options_format_label(int id, float value, char* out,
								 size_t out_size) {
	switch(id) {
		case OPTIONS_MUSIC:
		case OPTIONS_SOUND: {
			const char* name = id == OPTIONS_MUSIC ? "Music" : "Sound";
			if(value == 0.0F)
				snprintf(out, out_size, "%s: OFF", name);
			else
				snprintf(out, out_size, "%s: %d%%", name,
						 (int)(value * 100.0F));
			break;
		}

		case OPTIONS_SPLIT:
			/* The row never claims a state the server has not granted: while a
			 * request is outstanding it says so, and a request that failed or
			 * was never answered shows the ACKNOWLEDGED value with the reason
			 * on the helper line under it.
			 *
			 * S-A13 (Astra call #5): the failed / no-answer word is read from
			 * `split_applied` -- the last value the SERVER acknowledged --
			 * rather than from `split_value`. Every path into those two states
			 * already assigns `split_value = split_applied`, so this prints
			 * the same characters today; taking it from the acknowledged field
			 * makes the ruling ("ON/OFF must show the last acknowledged
			 * state") a property of the string selection instead of an
			 * invariant four call sites have to keep. SETTLED deliberately
			 * keeps `split_value`: with no server that is the player's pending
			 * edit, which is exactly what the title-screen row must show. */
			switch(split_row_state) {
				case SPLIT_ROW_PENDING:
					snprintf(out, out_size, "Local split-screen: Applying...");
					break;
				case SPLIT_ROW_FAILED:
					snprintf(out, out_size, "Local split-screen: %s (failed)",
							 split_applied ? "ON" : "OFF");
					break;
				case SPLIT_ROW_NO_ANSWER:
					snprintf(out, out_size,
							 "Local split-screen: %s (no answer)",
							 split_applied ? "ON" : "OFF");
					break;
				case SPLIT_ROW_SETTLED:
				default:
					snprintf(out, out_size, "Local split-screen: %s",
							 split_value ? "ON" : "OFF");
					break;
			}
			break;

		case OPTIONS_VIDEO:
			snprintf(out, out_size, "Display...");
			break;
		case OPTIONS_CONTROLS:
			snprintf(out, out_size, "Controller settings...");
			break;
		case OPTIONS_DIFFICULTY:
			/* GameSettings.getKeyBinding: "Difficulty: " + the
			 * options.difficulty.<name> translation (GameSettings.java:172).
			 * Remote session (Astra W1F ruling): the server owns the value,
			 * so the row names that instead of showing the local option as
			 * if it were in force. */
			if(gstate.remote_session)
				snprintf(out, out_size, "Controlled by server");
			else
				snprintf(out, out_size, "Difficulty: %s",
						 difficulty_name(difficulty_value));
			break;
		default:
			snprintf(out, out_size, "Back");
			break;
	}
}

/* The request goes to the LOCAL server only (an in-process value, never a
 * wire message), so a remote multiplayer server -- which owns its own
 * difficulty -- is never affected; the row is also disabled during a remote
 * session (options_target_enabled) and Options is unreachable from a remote
 * pause menu (screen_pause.c). With no world open the request is held and the
 * next local world starts from it. Split-screen host and guest are two bodies
 * in that one world, so they share the value. */
static void difficulty_apply(int value) {
	difficulty_value = difficulty_sanitize(value);
	server_local_request_difficulty(difficulty_value);
}

static void options_refresh_labels(void) {
	options_format_label(OPTIONS_MUSIC, music_value, labels[0],
						 sizeof(labels[0]));
	options_format_label(OPTIONS_SOUND, sound_value, labels[1],
						 sizeof(labels[1]));
	for(size_t k = OC_VIDEO; k < OPTIONS_COUNT; k++)
		options_format_label(controls[k].id, 0.0F, labels[k],
							 sizeof(labels[k]));
}

/* Compose the split row's helper line from a fixed prefix plus server text,
 * truncating with a visible "..." rather than a silent cut -- `tail` is the
 * server's own reason string and may be the full CRPC_SPLIT_REASON_CAP. */
static void split_status_set(const char* prefix, const char* tail) {
	char full[SPLIT_STATUS_CAP + CRPC_SPLIT_REASON_CAP + 8];
	snprintf(full, sizeof(full), "%s%s", prefix, tail ? tail : "");
	size_t len = strlen(full);
	if(len < SPLIT_STATUS_CAP) {
		memcpy(split_status, full, len + 1);
		return;
	}
	memcpy(split_status, full, SPLIT_STATUS_CAP - 4);
	memcpy(split_status + SPLIT_STATUS_CAP - 4, "...", 4);
}

/* What the config key must hold: the ACKNOWLEDGED value while a live request
 * is outstanding (the row shows what it is asking for, but the file must never
 * promise a state the server has not granted), the row's value otherwise --
 * which on the title screen is the player's pending edit, exactly as before. */
static int split_config_value(void) {
	return split_row_state == SPLIT_ROW_PENDING ? (split_applied ? 1 : 0)
												: split_value;
}

/* Move the acknowledged value into the IN-MEMORY config immediately -- an ack
 * can land while the player is already back in the game, so the value must not
 * wait for the row to be left -- but do NOT save here (R-A8 A8-M6). The poll
 * runs from main.c's per-frame ack seam, and config_save() is a synchronous
 * file write (libfat SD on the Wii); spending it inside a gameplay frame is
 * exactly the cost the review flagged, and it was never measured on Broadway.
 * The existing Options transaction (options_commit, the one that writes the
 * audio keys) persists it on the next leave, and because config_save()
 * serialises the whole object, so does any other menu commit.
 * `report` is true only for the ack that actually GRANTED the request: on a
 * refusal the helper line is carrying the server's own reason, and a set
 * failure must not overwrite the diagnosis (the review's cosmetic note). */
static void split_commit_key(bool report) {
	int stored = config_read_int(&gstate.config_user,
								 "experimental.local_splitscreen", 0) ? 1 : 0;
	if(stored == split_config_value()) {
		/* Already in the IN-MEMORY object, so there is nothing to set -- and
		 * NOTHING may be concluded about the FILE from that (R-A9 A9-H1).
		 * config_set_int writes the in-memory JSON root and config_read_int
		 * reads that same object back, so this test says "the object agrees",
		 * never "the object has been saved". Clearing split_key_dirty here
		 * disarmed the deferred write one frame after it was armed: the value
		 * was in memory, the file still held the old one, and
		 * screen_options_split_flush then walked away from a toggle the server
		 * had granted. split_key_dirty is cleared ONLY where a config_save
		 * actually succeeded -- options_commit and screen_options_split_flush
		 * -- or re-seeded by screen_options_split_boot. */
		return;
	}
	if(config_set_int(&gstate.config_user, "experimental.local_splitscreen",
					  split_config_value())) {
		split_key_dirty = true;
		return;
	}
	if(report)
		split_status_set("Applied; could not store the option.", NULL);
}

/* A live request is possible only for a LOCAL world that is actually loaded
 * and whose input handoff has been acknowledged: clin_input_epoch() is the
 * stamp the server validates, and a zero stamp is rejected by design, so a
 * request must not be sent before the first handoff. Options is already
 * unreachable from a remote session's pause menu (screen_pause.c), but this
 * states the requirement instead of relying on that. */
static bool split_live_available(void) {
	return gstate.world_loaded && !gstate.world_switch_pending
		&& !gstate.remote_session && gstate.players[0].local_player
		&& clin_input_epoch() != 0;
}

/* ---- The focused row's two helper lines (Astra call #5 wording, S-A13) ---
 * The helper block under the row is TWO lines at fixed positions (see
 * screen_options_render2D); this picks what each one says, and nothing else.
 * No metric moves: same two lines, same y, same font, same colours.
 *
 * Line 1 is the transient/marker line. A live request's status owns it
 * whenever there is one ("Applying...", "Could not apply: <reason>", "No
 * answer from the server.", "Could not send: ..."), because that is the
 * feedback the player is waiting for. With no status it states what a press
 * does, TRUTHFULLY for where the player is: in a world the switch applies
 * through the server-owned transition ("Experimental - applies immediately."),
 * while on the title screen nothing is asked of anybody and the boot/world-load
 * seed applies the value ("Applies when the next world loads.") -- the ruling's
 * "title-screen wording must describe its actual behavior".
 *
 * Line 2 is the join-availability line, expressed against the ACKNOWLEDGED
 * switch (`split_applied`) so it can never promise what the server has not
 * granted. Player 2's presence is the predicate screen_pause.c and
 * screen_ingame.c already use for the same question -- a guest BODY installed
 * on the client -- so no new getter is needed here.
 *
 * WHY THE ENABLED HELPER USES BOTH LINES. The approved availability string is
 * 71 characters; the helper line's drawable budget is 48 (SPLIT_STATUS_CAP,
 * the width the menu proves it can draw). Truncating it with "..." -- the
 * treatment the server's reason string gets -- would cut the view-distance
 * consequence off the end, which is the half a player needs BEFORE turning the
 * switch on. Its two sentences are 25 and 45 characters, so the approved text
 * is shown in full across the block's existing two lines instead, and the
 * marker stands down for that one state (the ruling permits the marker where
 * application is live, it does not require it). Nothing is truncated, and
 * nothing here is a new string. */
static void split_helper_lines(const char** line1, const char** line2) {
	bool live = split_live_available();
	/* A guest BODY on the client -- the same test screen_pause.c uses for its
	 * player-2 connection notice. A join still in flight is not "present": it
	 * has no body yet and may still be refused. */
	bool guest = gstate.players[1].local_player != NULL;
	const char* join = "When active, second controller presses A to join.";
	if(live && !split_applied && guest)
		/* OFF with player 2 already playing: the switch closed NEW joins, and
		 * the server never ejects an admitted guest (see
		 * screen_options_split_guest_maintain). No confirmation is asked. */
		join = "New joins disabled. Player 2 can keep playing.";
	if(split_status[0]) {
		*line1 = split_status;
		*line2 = join;
		return;
	}
	if(live && split_applied && !guest) {
		*line1 = "A second player can join.";
		*line2 = "View distance is reduced during split-screen.";
		return;
	}
	*line1 = live ? "Experimental - applies immediately."
				  : "Applies when the next world loads.";
	*line2 = join;
}

/* The ONE entry point every way the row changes value funnels through (A,
 * Left, Right). `wanted` is a DESIRED state, not a toggle -- the same shape
 * SRPC_SPLIT_SET carries, so a duplicate press is idempotent. Returns whether
 * the press did anything (the caller's click cue). */
static bool split_row_set_internal(int wanted, bool force_request) {
	wanted = wanted ? 1 : 0;
	if(!split_live_available()) {
		/* Title screen / no world: unchanged behaviour. No request, no pending
		 * state -- the value is committed with the other Options keys and the
		 * boot path applies it at the next world load. */
		if(wanted == split_value)
			return false;
		split_value = wanted;
		split_row_state = SPLIT_ROW_SETTLED;
		split_status[0] = '\0';
		options_refresh_labels();
		return true;
	}
	if(!force_request && split_row_state == SPLIT_ROW_PENDING) {
		if(wanted == (int)split_pending_want)
			return false; /* already asking for exactly this */
	} else if(!force_request && split_row_state == SPLIT_ROW_SETTLED
			  && wanted == (int)split_applied) {
		return false; /* already there, and the server agreed */
	}
	/* A newer toggle while one is pending SUPERSEDES it: a fresh request_seq,
	 * so the older request's ack is ignored when it arrives. */
	struct server_rpc call = {.type = SRPC_SPLIT_SET,
		.payload.split_set = {.enabled = wanted != 0,
			.world_generation = clin_input_epoch(),
			.request_seq = split_request_seq + 1}};
	if(!svin_rpc_send(&call)) {
		/* The dedicated reserve slot is taken and the coalescer could not fold
		 * this into a pending request -- say so instead of showing a state
		 * nobody asked for. The request NEVER LEFT, so nothing about it may be
		 * recorded: split_request_seq is not advanced (the number is still
		 * unused) and split_pending_seq is CLEARED (R-A8 A8-H2 -- leaving an
		 * older seq there while the row said FAILED was the second half of the
		 * "server ON, client OFF forever" divergence: the poll's old row-state
		 * guard then dropped that older request's ack). Any ack still owed to
		 * an earlier request is now handled by the generation reconcile in
		 * screen_options_split_poll, which no longer needs a pending seq. */
		split_value = split_applied ? 1 : 0;
		split_row_state = SPLIT_ROW_FAILED;
		split_status_set("Could not send: ", "transport busy");
		/* A request that never left is a SETTLED failure, not a pending one:
		 * the heartbeat says so rather than leaving last_ok at "nothing has
		 * happened". There is no round trip to report. */
		split_last_ok = 0;
		split_request_stamped = false;
		split_pending_seq = 0;
		split_pending_age = 0.0F;
		options_refresh_labels();
		return true;
	}
	/* Stamped AFTER the send succeeded, so the measured interval is the
	 * server's answer time and not this frame's own transport work. */
	split_request_sent = time_get();
	split_request_stamped = true;
	split_request_seq++;
	split_pending_seq = split_request_seq;
	split_pending_want = wanted != 0;
	split_pending_age = 0.0F;
	split_row_state = SPLIT_ROW_PENDING;
	split_value = wanted;
	split_status_set("Applying...", NULL);
	options_refresh_labels();
	return true;
}

static bool split_row_set(int wanted) {
	return split_row_set_internal(wanted, false);
}

static void split_off_reset(void) {
	split_off = (struct split_off_transaction) {0};
}

enum screen_options_split_off_phase screen_options_split_off_phase(void) {
	return split_off.phase;
}

void screen_options_split_off_cancel(void) {
	if(split_off.phase == SCREEN_SPLIT_OFF_CONFIRM)
		split_off_reset();
}

static void split_off_request(void) {
	uint32_t before = split_request_seq;
	(void)split_row_set_internal(0, true);
	if(split_request_seq != before && split_row_state == SPLIT_ROW_PENDING
	   && split_pending_seq == split_request_seq) {
		split_off.split_request_seq = split_request_seq;
		split_off.phase = SCREEN_SPLIT_OFF_REQUEST_PENDING;
	} else {
		split_off.phase = SCREEN_SPLIT_OFF_REQUEST_FAILED;
		split_off.selected = 1;
	}
}

static bool split_off_open(void) {
	if(split_off.phase != SCREEN_SPLIT_OFF_IDLE || !split_live_available()
	   || !split_applied || split_row_state == SPLIT_ROW_PENDING)
		return false;
	split_off_reset();
	split_off.phase = SCREEN_SPLIT_OFF_CONFIRM;
	split_off.world_generation = clin_input_epoch();
	split_off.selected = 0; /* Keep on is the safe default. */
	split_off.focus = (struct gutil_menu_focus) {0};
	split_off.origin = GUTIL_MENU_ORIGIN_DEFAULT;
	return true;
}

static bool split_ui_set(int wanted) {
	if(!wanted && split_live_available() && split_applied)
		return split_off_open();
	return split_row_set(wanted);
}

bool screen_options_split_set(bool enabled) {
	return split_ui_set(enabled ? 1 : 0);
}

const char* screen_options_split_label(void) {
	/* Compact host Options can be the first Options surface opened after boot,
	 * before the full-screen setup has populated its label cache. Format this
	 * one row from the live state at the point of use so that fresh-world Pause
	 * never renders an empty button. */
	options_format_label(OPTIONS_SPLIT, 0.0F, labels[OC_SPLIT],
					 sizeof(labels[OC_SPLIT]));
	return labels[OC_SPLIT];
}

static void split_off_confirm(struct client_session* guest) {
	if(!guest || split_off.world_generation == 0
	   || clin_input_epoch() != split_off.world_generation) {
		split_off_reset();
		return;
	}
	if(guest->local_phase == CLIENT_GUEST_IDLE) {
		split_off_request();
		return;
	}
	if(client_session_local_leave_begin(guest, &split_off.leave))
		split_off.phase = SCREEN_SPLIT_OFF_LEAVE_PENDING;
	else {
		split_off.phase = SCREEN_SPLIT_OFF_LEAVE_FAILED;
		split_off.selected = 1;
	}
}

void screen_options_split_off_confirm(struct client_session* guest) {
	if(split_off.phase == SCREEN_SPLIT_OFF_CONFIRM)
		split_off_confirm(guest);
}

static void split_off_release_leave(struct client_session* guest) {
	if(guest && split_off.leave.ticket != 0)
		(void)client_session_local_leave_release(guest, &split_off.leave);
}

bool screen_options_split_off_blocks_join(void) {
	return split_off.phase == SCREEN_SPLIT_OFF_LEAVE_PENDING
		|| split_off.phase == SCREEN_SPLIT_OFF_LEAVE_FAILED
		|| split_off.phase == SCREEN_SPLIT_OFF_REQUEST_PENDING
		|| split_off.phase == SCREEN_SPLIT_OFF_REQUEST_FAILED;
}

void screen_options_split_off_tick(struct client_session* guest,
								   uint32_t world_generation,
								   const struct clin_split_ack* ack) {
	if(split_off.phase == SCREEN_SPLIT_OFF_IDLE)
		return;
	if(world_generation == 0 || world_generation != split_off.world_generation) {
		split_off_release_leave(guest);
		split_off_reset();
		split_off_rearm = true;
		return;
	}
	if(split_off.phase == SCREEN_SPLIT_OFF_CONFIRM)
		return;
	if(split_off.phase == SCREEN_SPLIT_OFF_LEAVE_PENDING
	   || split_off.phase == SCREEN_SPLIT_OFF_LEAVE_FAILED) {
		enum client_local_leave_outcome outcome =
			client_session_local_leave_observe(guest, &split_off.leave);
		if(outcome == CLIENT_LOCAL_LEAVE_LEFT) {
			split_off_release_leave(guest);
			split_off_request();
		} else if(outcome == CLIENT_LOCAL_LEAVE_PENDING) {
			split_off.phase = SCREEN_SPLIT_OFF_LEAVE_PENDING;
		} else if(outcome == CLIENT_LOCAL_LEAVE_FAILED
				|| outcome == CLIENT_LOCAL_LEAVE_TIMED_OUT) {
			split_off.phase = SCREEN_SPLIT_OFF_LEAVE_FAILED;
			split_off.selected = 1;
		} else if(outcome == CLIENT_LOCAL_LEAVE_STALE) {
			split_off_reset();
			split_off_rearm = true;
		}
		return;
	}
	if(split_off.phase != SCREEN_SPLIT_OFF_REQUEST_PENDING
	   && split_off.phase != SCREEN_SPLIT_OFF_REQUEST_FAILED)
		return;
	if(split_off.split_request_seq != 0 && ack && ack->valid
	   && ack->generation == split_off.world_generation
	   && ack->request_seq == split_off.split_request_seq) {
		if(ack->ok && !ack->enabled) {
			split_off_reset();
			split_off_rearm = true;
		} else {
			split_off.phase = SCREEN_SPLIT_OFF_REQUEST_FAILED;
			split_off.selected = 1;
		}
		return;
	}
	if(split_off.phase == SCREEN_SPLIT_OFF_REQUEST_PENDING
	   && (split_row_state == SPLIT_ROW_FAILED
		   || split_row_state == SPLIT_ROW_NO_ANSWER)) {
		split_off.phase = SCREEN_SPLIT_OFF_REQUEST_FAILED;
		split_off.selected = 1;
	}
}

void screen_options_split_boot(bool applied) {
	split_off_reset();
	split_off_rearm = false;
	split_applied = applied;
	split_applied_seeded = true;
	split_value = applied ? 1 : 0;
	split_row_state = SPLIT_ROW_SETTLED;
	split_status[0] = '\0';
	split_pending_seq = 0;
	split_pending_age = 0.0F;
	split_key_dirty = false;
	/* The heartbeat evidence is per-WORLD: a fresh boot has not toggled
	 * anything yet, and carrying a previous world's round trip into it would
	 * make the stream claim a toggle this world never saw. */
	split_request_stamped = false;
	split_toggle_ms = 0;
	split_last_ok = -1;
	/* split_request_seq is deliberately NOT reset: it stays monotonic for the
	 * whole session so an ack for a retired request can never match a later
	 * one. */
	split_seed_generation = 0;
	options_refresh_labels();
}

/* ---- Per-world re-seed of the live switch (R-A9 A9-M5) -------------------
 * The boot path reads experimental.local_splitscreen ONCE, before the main
 * loop, and applies it with server_local_set_split_enabled while the server
 * thread is still stopped. Nothing re-read the file after that, so a
 * title-screen edit reached the file and the NEXT LAUNCH but not any world
 * this process went on to load: the player's explicit OFF was ignored for the
 * rest of the session (the row snapped back to the boot-applied value on
 * reopen, input stayed armed, and a second controller could still join).
 *
 * This is that re-seed, once per WORLD GENERATION. main.c calls it every frame
 * at the ack seam; it is a single epoch compare until the generation changes.
 *
 * WHY THE REQUEST PATH AND NOT THE BOOT SETTER. server_local_set_split_enabled
 * REFUSES once s->thread_started (server_local.c) -- by design: the server
 * thread owns s->split_enabled, and a main-thread write mid-session would race
 * it. So the only way to re-seed the SERVER consistently with the client is
 * the live route, SRPC_SPLIT_SET, which applies the same transition on the
 * owning thread and answers with an ack. That keeps the ruling's order intact:
 * the row ASKS for the file's value, and `split_applied` -- what input arming
 * follows -- still moves only when the server states it. There is no
 * client-side path here that sets split_applied from the file.
 *
 * `force_on` is main.c's `hw_run.enabled && hw_run.split_guest` term, the same
 * disjunct the boot seed uses: an armed split harness profile must not have
 * its forced ON asked away by a config file that says 0.
 *
 * Returns the value input arming must follow, exactly like
 * screen_options_split_poll -- the ACKNOWLEDGED switch, never the file. */
bool screen_options_split_world_seed(bool force_on) {
	uint32_t generation = clin_input_epoch();
	/* split_live_available() is the same gate a press funnels through: a world
	 * actually loaded, local, with an ACKED epoch. Until it is true there is no
	 * generation to seed and no legal request to send, so this waits (the
	 * epoch is 0 between worlds, which can never equal a seeded generation). */
	if(!split_live_available() || generation == split_seed_generation)
		return split_applied;
	split_seed_generation = generation;
	/* Per-WORLD evidence, reset for the same reason screen_options_split_boot
	 * resets it: carrying the previous world's round trip or outcome into this
	 * one would make the heartbeat claim a toggle THIS world never saw. */
	split_request_stamped = false;
	split_toggle_ms = 0;
	split_last_ok = -1;
	/* No stale live state crosses a generation. A toggle still in flight when
	 * the world went away can never be answered -- every ack is
	 * generation-stamped and split_ack_current rejects another generation's --
	 * so leaving the pending seq there would strand the row until its 5 s
	 * timeout and then report a failure against the NEW world. The row starts
	 * this world SETTLED on the acknowledged switch with a clean caption. */
	split_pending_seq = 0;
	split_pending_age = 0.0F;
	split_row_state = SPLIT_ROW_SETTLED;
	split_status[0] = '\0';
	split_value = split_applied ? 1 : 0;
	/* split_key_dirty is deliberately NOT cleared: if the previous world's
	 * teardown flush failed, that write is still owed and must survive into
	 * this world (screen_options_split_boot may clear it -- it runs before any
	 * toggle exists -- but a world load may not). */
	int want = (config_read_int(&gstate.config_user,
								"experimental.local_splitscreen", 0)
					   != 0
				   || force_on)
		? 1
		: 0;
	if(want != (split_applied ? 1 : 0))
		/* The file disagrees with the server's last stated switch: ask. One
		 * attempt per generation -- a refused send leaves the row's own
		 * "Could not send" caption, which is the truth, and the player can
		 * retry from the row; retrying here every frame would storm it. */
		(void)split_row_set(want);
	else
		options_refresh_labels();
	return split_applied;
}

/* Is this ack a statement about the world the client is looking at RIGHT NOW?
 * The server stamps every CRPC_SPLIT_STATE with its OWN generation
 * (s->input_epoch_counter), and clin_input_epoch() is the last generation this
 * client acknowledged, so equality means the two agree about which world is
 * loaded. Zero on either side is the reserved "no handoff yet" value and can
 * never match. */
static bool split_ack_current(const struct clin_split_ack* ack) {
	uint32_t epoch = clin_input_epoch();
	return epoch != 0 && ack->generation == epoch;
}

bool screen_options_split_poll(const struct clin_split_ack* ack, float dt) {
	/* ---- Reconcile to the SERVER, whatever the row state (R-A8 A8-H1/H2) ---
	 * The row used to look at an ack ONLY while it was PENDING, so every exit
	 * from PENDING that was not "the matching ack arrived" -- the 5 s timeout,
	 * a refused svin_rpc_send -- stranded the answer permanently: the server
	 * was ON with its residency block allocated and its accept class armed
	 * while the client believed OFF, for the rest of the session, with no way
	 * back. The server is the authority for the live state, so an ack that
	 * names the CURRENT generation is adopted from any row state: the
	 * acknowledged value it carries is `s->split_enabled` itself, read after
	 * the transition it is answering.
	 *
	 * CONTRACT (R-A9 A9-H1/H2/M3): `ack` is a NEW server statement or NULL.
	 * The caller must hand each ack over exactly once -- clin_split_last_ack
	 * consumes the slot, which is what guarantees it. Re-feeding one ack frame
	 * after frame turns every branch below into a per-frame event: the
	 * deferred config write gets armed and cancelled, the timeout at the
	 * bottom is never reached because the adopt branch returns first, and one
	 * caption is pinned over every later row state. */
	if(ack && ack->valid && split_ack_current(ack)) {
		bool matching = split_pending_seq != 0
			&& ack->request_seq == split_pending_seq;
		/* The round trip, measured before anything else so a slow commit or
		 * label refresh below cannot land in the number. Clamped at 1 ms
		 * because 0 is the "no toggle answered yet" sentinel, and floored at
		 * 0 against a non-monotonic clock read. Only for the request this row
		 * is actually waiting on -- a reconcile is not a round trip. */
		if(matching && split_request_stamped) {
			int32_t rt = time_diff_ms(split_request_sent, time_get());
			split_toggle_ms = rt > 0 ? (uint32_t)rt : 1u;
			split_request_stamped = false;
		}
		/* The ACKNOWLEDGED value comes from EVERY current-generation ack, `ok`
		 * or not (R-A9 A9-M6). The server stamps `.enabled` with
		 * `s->split_enabled` read AFTER the transition the ack answers --
		 * refusals included -- so the field is the server's own switch by
		 * construction, and the ack slot is a single last-wins struct with no
		 * queue: a GRANT answered in the same drain as a later REFUSAL is
		 * overwritten, and adopting only inside the `ok` branch left the
		 * client OFF while the server was ON with nothing able to heal it.
		 * What `ok` decides is what happens to the ROW, not what the switch
		 * IS. */
		bool changed = split_applied != (bool)ack->enabled;
		split_applied = ack->enabled;
		if(ack->ok) {
			split_last_ok = 1;
			if(matching || split_row_state != SPLIT_ROW_PENDING) {
				/* The row's own decision is over: either this is its ack, or
				 * the row was not waiting for one at all and a FAILED /
				 * NO_ANSWER caption is now stale. */
				split_row_state = SPLIT_ROW_SETTLED;
				split_status[0] = '\0';
				split_value = split_applied ? 1 : 0;
				split_pending_seq = 0;
				changed = true;
			}
			/* ...otherwise the row keeps waiting on its own newer seq (it is
			 * still "Applying..."), but the ACKNOWLEDGED state below it has
			 * already caught up. */
			split_commit_key(true);
		} else if(matching) {
			/* ok=false for the request the row IS waiting on: the transition
			 * did not happen, so the toggle is retired with the server's own
			 * reason and the row falls back to the acknowledged value. */
			split_last_ok = 0;
			split_status_set("Could not apply: ",
							 ack->reason[0] ? ack->reason : "refused");
			split_row_state = SPLIT_ROW_FAILED;
			split_value = split_applied ? 1 : 0;
			split_pending_seq = 0;
			split_commit_key(false);
			changed = true;
		} else if(split_row_state == SPLIT_ROW_NO_ANSWER) {
			/* The row already gave up on a request and says "No answer from
			 * the server." A refusal arriving now is the answer it never got,
			 * so the server's reason replaces that caption -- strictly more
			 * truthful, and it cannot corrupt the row because NO_ANSWER
			 * already shows the acknowledged value. */
			split_last_ok = 0;
			split_status_set("Could not apply: ",
							 ack->reason[0] ? ack->reason : "refused");
			split_row_state = SPLIT_ROW_FAILED;
			split_value = split_applied ? 1 : 0;
			split_commit_key(false);
			changed = true;
		} else if(changed) {
			/* A refusal that answers NOTHING this row is tracking (a
			 * superseded request's, or the server's `request_seq == 0`
			 * "invalid request" answer to a caller that is not this row --
			 * R-A9 A9-L7) still REPORTS the switch, so the reported value is
			 * followed without inventing a failure the player did not cause.
			 * PENDING keeps showing what it is asking for; the acknowledged
			 * state underneath it has moved either way. */
			if(split_row_state != SPLIT_ROW_PENDING)
				split_value = split_applied ? 1 : 0;
			split_commit_key(false);
		}
		/* ...and a refusal that matches nothing and moves nothing changes not
		 * one field: it must not flip a healthy SETTLED row to FAILED, and it
		 * must not overwrite an in-flight "Applying..." or a FAILED / "Could
		 * not send" caption the player has not read yet (R-A9 A9-H2 b/c).
		 * Label reformatting follows the same rule -- OPTIONS_COUNT snprintfs
		 * only when something actually moved (R-A9 A9-M3). */
		if(changed)
			options_refresh_labels();
		return split_applied;
	}
	if(split_row_state != SPLIT_ROW_PENDING)
		return split_applied;
	/* Any other ack -- one for a different generation, or none at all --
	 * leaves the pending decision outstanding. */
	if(dt > 0.0F)
		split_pending_age += dt;
	if(split_pending_age >= SPLIT_ACK_TIMEOUT_S) {
		split_row_state = SPLIT_ROW_NO_ANSWER;
		split_status_set("No answer from the server.", NULL);
		split_value = split_applied ? 1 : 0; /* previous state kept */
		split_pending_seq = 0;
		/* A lost request is NOT a success, so the heartbeat reports 0 -- but
		 * there was no round trip, so split_toggle_ms keeps whatever the last
		 * ANSWERED toggle measured (see the field comment): reporting the
		 * 5,000 ms timeout as a round trip would invent a latency the server
		 * never produced. */
		split_last_ok = 0;
		split_request_stamped = false;
		options_refresh_labels();
	}
	return split_applied;
}

/* ---- Client-side guest maintenance under the LIVE switch (R-A8 A8-H3) ----
 * "OFF never ejects a guest" was true only on the SERVER. On the client the
 * whole guest-maintenance block in main.c was gated on the switch, so a live
 * OFF also switched off the join settle, the screen pin and the guest ANCHOR
 * update -- wedging a guest that was ATTACHED but not yet admitted (its join
 * card never retired, its controller still claimed, no failure path out) and
 * freezing s_guest_anchor_x/z at the position the body had when the switch
 * flipped, which silently corrupts every later anchor-1 measurement.
 *
 * These two predicates are the fix, expressed against the ACKNOWLEDGED switch
 * (`split_applied`) that this file owns, so main.c cannot feed them a value the
 * server has not granted. They are pure: no state moves here.
 *
 * screen_options_split_guest_maintain -- must the client keep maintaining the
 *   guest half this frame? Yes while the switch is on, and yes while a guest
 *   SESSION or BODY still exists whatever the switch says. With the switch off
 *   and no guest at all it is false, so the single-player frame is untouched.
 *
 * screen_options_split_guest_refuse -- must an unfinished join be settled as
 *   REFUSED and its controller claim released? Only when the switch is off and
 *   a join is still trying without an admitted body. The decision is safe to
 *   take on the very frame the OFF ack lands, because the request and the ack
 *   share one FIFO in each direction: a gameplay request the server GRANTED
 *   before it applied the OFF put its LOCAL_GUEST_GAMEPLAY_READY in the client
 *   inbox ahead of the CRPC_SPLIT_STATE, and clin_update drains the whole inbox
 *   in one call -- so `admitted` is already true here in that case, and the
 *   body is left completely alone (the server invariant: OFF never ejects a
 *   guest already playing). Any request the server had NOT granted by then is
 *   refused by its admission gate, which OFF closes for every NEW body. */
bool screen_options_split_guest_maintain(bool session_exists, bool admitted) {
	return split_applied || session_exists || admitted;
}

bool screen_options_split_guest_refuse(bool joining, bool admitted) {
	return !split_applied && joining && !admitted;
}

/* Read-only snapshot for the heartbeat (Astra H3, S-A3). Main/client thread,
 * the same thread that owns every field above -- no locking, and no write
 * path: a caller can only observe. See struct split_live_status in screen.h
 * for what each field means. */
struct split_live_status screen_options_split_live_status(void) {
	struct split_live_status s = {
		.live = split_applied,
		.pending = split_row_state == SPLIT_ROW_PENDING,
		.toggle_ms = split_toggle_ms,
		.last_ok = split_last_ok,
	};
	return s;
}

/* Approved studio.js geometry, scaled from its 640 x 480 canvas. */
static int opt_px(int v) { return v * gfx_height() / 480; }
static struct fep_rect opt_rect(int x, int y, int w, int h) {
	return fep_rect((gfx_width() - opt_px(640)) / 2 + opt_px(x),
		opt_px(y), opt_px(w), opt_px(h));
}
static struct fep_rect split_off_panel(void) {
	return opt_rect(84, 138, 472, 220);
}
static struct fep_rect split_off_button(int index) {
	return opt_rect(index ? 332 : 108, 280, 196, 44);
}

bool screen_options_split_off_retry(struct client_session* guest) {
	if(split_off.phase == SCREEN_SPLIT_OFF_LEAVE_FAILED) {
		if(client_session_local_leave_retry(guest, &split_off.leave)) {
			split_off.phase = SCREEN_SPLIT_OFF_LEAVE_PENDING;
			return true;
		}
		return false;
	}
	if(split_off.phase == SCREEN_SPLIT_OFF_REQUEST_FAILED) {
		split_off_request();
		return split_off.phase == SCREEN_SPLIT_OFF_REQUEST_PENDING;
	}
	return false;
}

/* The one activation question the split-OFF modal asks, built in one place
 * so the test hook below reads exactly what the press reads. */
static struct gutil_menu_activate_state split_off_activate_state(bool click,
		bool navigating, bool actionable, int first) {
	return (struct gutil_menu_activate_state) {
			.pressed = click && actionable,
			.navigated = navigating,
			.pointer_valid = pointer_available,
			.selection_origin = split_off.origin,
			.hovered = pointer_target,
			.hovered_enabled = pointer_target >= first,
			.selected = split_off.selected,
			.selected_enabled = actionable && split_off.selected >= first,
			/* Turn off ends Player 2's session: destructive (R-D) and a
			 * destructive confirmation (Astra D2-destructive ruling B) --
			 * a sprite/ring mismatch only moves focus. The Retry phases
			 * have one button and keep the plain rule. */
			.selected_destructive = split_off.phase == SCREEN_SPLIT_OFF_CONFIRM
				&& split_off.selected == 1,
			.destructive_dialog = split_off.phase == SCREEN_SPLIT_OFF_CONFIRM,
		};
}

static bool split_off_modal_update(void) {
	if(split_off.phase == SCREEN_SPLIT_OFF_IDLE)
		return false;
	pointer_available = input_pointer(&pointer_x, &pointer_y, &pointer_angle);
	bool left = input_pressed(IB_GUI_LEFT);
	bool right = input_pressed(IB_GUI_RIGHT);
	bool navigating = left || right || input_pressed(IB_GUI_UP)
		|| input_pressed(IB_GUI_DOWN);
	bool owner = gutil_menu_pointer_focus(&split_off.focus, pointer_available,
		pointer_x, pointer_y, navigating);
	pointer_target = -1;
	int first = split_off.phase == SCREEN_SPLIT_OFF_CONFIRM ? 0 : 1;
	int last = 1;
	if((split_off.phase == SCREEN_SPLIT_OFF_CONFIRM
		|| split_off.phase == SCREEN_SPLIT_OFF_LEAVE_FAILED
		|| split_off.phase == SCREEN_SPLIT_OFF_REQUEST_FAILED) && pointer_available) {
		for(int i = first; i <= last; i++) {
			if(fep_rect_hit(split_off_button(i), pointer_x, pointer_y)) {
				pointer_target = i;
				if(owner) {
					split_off.selected = i;
					split_off.origin = GUTIL_MENU_ORIGIN_POINTER;
				}
				break;
			}
		}
	}
	if(split_off.phase == SCREEN_SPLIT_OFF_CONFIRM && navigating) {
		split_off.selected = right ? 1 : left ? 0 : split_off.selected;
		split_off.origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	}
	bool actionable = split_off.phase == SCREEN_SPLIT_OFF_CONFIRM
		|| split_off.phase == SCREEN_SPLIT_OFF_LEAVE_FAILED
		|| split_off.phase == SCREEN_SPLIT_OFF_REQUEST_FAILED;
	bool click = input_pressed(IB_GUI_CLICK);
	struct gutil_menu_decision d = gutil_menu_activate_decide(
		split_off_activate_state(click, navigating, actionable, first));
	split_off.selected = d.selected;
	split_off.origin = d.selection_origin;
	if(d.action == GUTIL_MENU_ACTIVATE) {
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		if(split_off.phase == SCREEN_SPLIT_OFF_CONFIRM) {
			if(d.target == 0) screen_options_split_off_cancel();
			else screen_options_split_off_confirm(&gstate.players[1]);
		} else
			(void)screen_options_split_off_retry(&gstate.players[1]);
		return true;
	}
	if(!click && split_off.phase == SCREEN_SPLIT_OFF_CONFIRM
	   && (input_pressed(IB_GUI_CLICK_ALT) || input_pressed(IB_HOME)
		   || input_pressed(IB_INVENTORY))) {
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		screen_options_split_off_cancel();
	}
	return true;
}
static struct fep_rect opt_footer(bool draw) {
	/* The centred hint row (controller UI redesign 10-31, A1 helper); the
	 * Back hit target is the row's own Back cell, so pointer == drawn. */
	struct input_player_frame prompts;
	input_prompt_frame(0, &prompts);
	static const struct gutil_hint_item items[] = {
		{{IB_GUI_UP}, 1, "Move", 1, false},
		{{IB_GUI_CLICK}, 1, "Select", 2, false},
		{{IB_INVENTORY}, 1, "Back", 0, true}};
	struct gutil_hint_row row;
	(void)gutil_hint_row_layout(&prompts, 0, 0, gfx_width(), gfx_height(),
		false, GUTIL_HINT_ALIGN_CENTRE, items, 3, &row);
	struct fep_rect back = fep_rect(0, 0, 0, 0);
	for(int k = 0; k < row.count; k++)
		if(row.cells[k].item == 2)
			back = fep_rect(row.cells[k].rect.x, row.cells[k].rect.y,
				row.cells[k].rect.width, row.cells[k].rect.height);
	if(draw)
		gutil_hint_row_draw(&row);
	return back;
}
static void opt_text(int x, int y, int size, int room, const char* text, bool dark) {
	char fit[192];
	fep_text_fit(fit, sizeof(fit), text, opt_px(size), opt_px(room));
	struct fep_rect r = opt_rect(x, y, 0, 0);
	gutil_text_rgb(r.x, r.y, fit, opt_px(size), !dark,
		dark ? 32 : 255, dark ? 32 : 255, dark ? 32 : 255);
}
static void opt_button(struct fep_rect r, const char* text, bool enabled, bool focused) {
	fep_widget(r, focused && enabled);
	fep_bevel(r);
	if(!enabled) fep_fill(r, 0, 0, 0, 100);
	if(focused) fep_outline(r, opt_px(3), 255, 255, 255, 255);
	int size = opt_px(20);
	gutil_text_rgb(r.x + (r.w - gutil_font_width(text, size)) / 2,
		r.y + (r.h - size) / 2, text, size, true,
			enabled ? 255 : 160, enabled ? 255 : 160, enabled ? 255 : 160);
}

/* opt_button, but the label steps down from the 20-px button size (never
 * below 14) until it fits inside the button with a 6-px margin each side.
 * Only the half-width Difficulty button needs it: "Controlled by server" is
 * about 267 px at 20 px against a 258-px half. */
static void opt_button_fit(struct fep_rect r, const char* text, bool enabled,
						   bool focused) {
	fep_widget(r, focused && enabled);
	fep_bevel(r);
	if(!enabled) fep_fill(r, 0, 0, 0, 100);
	if(focused) fep_outline(r, opt_px(3), 255, 255, 255, 255);
	int size = opt_px(20);
	while(size > opt_px(14) && gutil_font_width(text, size) > r.w - opt_px(12))
		size--;
	gutil_text_rgb(r.x + (r.w - gutil_font_width(text, size)) / 2,
		r.y + (r.h - size) / 2, text, size, true,
			enabled ? 255 : 160, enabled ? 255 : 160, enabled ? 255 : 160);
}

static void split_off_modal_render(int width, int height) {
	if(split_off.phase == SCREEN_SPLIT_OFF_IDLE)
		return;
	fep_fill(fep_rect(0, 0, width, height), 0, 0, 0, 180);
	struct fep_rect panel = split_off_panel();
	fep_paper(panel);
	fep_bevel(panel);
	opt_text(108, 161, 23, 424, "Turn off split-screen?", true);
	const char* body = "Player 2 will leave the local session.";
	if(split_off.phase == SCREEN_SPLIT_OFF_LEAVE_PENDING)
		body = "Waiting for Player 2 to leave...";
	else if(split_off.phase == SCREEN_SPLIT_OFF_LEAVE_FAILED)
		body = "Player 2 could not leave. Retry to continue.";
	else if(split_off.phase == SCREEN_SPLIT_OFF_REQUEST_PENDING)
		body = "Player 2 left. Turning split-screen off...";
	else if(split_off.phase == SCREEN_SPLIT_OFF_REQUEST_FAILED)
		body = "Player 2 left. The server did not confirm Off.";
	opt_text(108, 205, 15, 424, body, true);
	if(split_off.phase == SCREEN_SPLIT_OFF_CONFIRM) {
		opt_button(split_off_button(0), "Keep on", true,
				   split_off.selected == 0);
		opt_button(split_off_button(1), "Turn off", true,
				   split_off.selected == 1);
	} else if(split_off.phase == SCREEN_SPLIT_OFF_LEAVE_FAILED) {
		opt_button(split_off_button(1), "Retry leave", true, true);
	} else if(split_off.phase == SCREEN_SPLIT_OFF_REQUEST_FAILED) {
		opt_button(split_off_button(1), "Retry turn off", true, true);
	}
}
/* Existing targets 0..4 and plus targets 5/6 are a test/input contract. The
 * new Controller settings row is appended as target 7 even though it appears
 * between Display and Split on screen. */
enum {
	OT_MUSIC_PLUS = 5,
	OT_SOUND_PLUS = 6,
	OT_CONTROLS = 7,
	/* W1F: appended; drawn as the right half of the Display row. */
	OT_DIFFICULTY = 8,
	OT_COUNT,
};
static int options_row(int target) {
	if(target == OT_MUSIC_PLUS)
		return OC_MUSIC;
	if(target == OT_SOUND_PLUS)
		return OC_SOUND;
	if(target == OT_CONTROLS)
		return OC_CONTROLS;
	if(target == OT_DIFFICULTY)
		return OC_DIFFICULTY;
	return target;
}
static const int visual_targets[] = {
	OC_MUSIC, OC_SOUND, OC_VIDEO, OT_CONTROLS, OC_SPLIT, OC_DONE,
};
static int options_visual_row(int target) {
	int row = options_row(target);
	/* Difficulty shares the Display row (right half): Up/Down leave it
	 * exactly as they leave Display. */
	if(row == OC_DIFFICULTY)
		row = OC_VIDEO;
	for(int i = 0; i < (int)(sizeof(visual_targets) / sizeof(visual_targets[0]));
		i++) {
		if(options_row(visual_targets[i]) == row)
			return i;
	}
	return 0;
}
static struct fep_rect options_target_rect(int target) {
	switch(target) {
	case OC_MUSIC: return opt_rect(332, 98, 60, 44);
	case OC_SOUND: return opt_rect(332, 160, 60, 44);
	case OT_MUSIC_PLUS: return opt_rect(524, 98, 60, 44);
	case OT_SOUND_PLUS: return opt_rect(524, 160, 60, 44);
	/* W1F: Display gives up the right half of its row to Difficulty --
	 * Beta's own GuiSmallButton pair form (GuiOptions.java:24, two 150 px
	 * buttons per row); every other approved rect is unchanged. */
	case OC_VIDEO: return opt_rect(56, 222, 258, 44);
	case OT_DIFFICULTY: return opt_rect(326, 222, 258, 44);
	case OT_CONTROLS: return opt_rect(56, 276, 528, 44);
	case OC_SPLIT: return opt_rect(476, 338, 108, 44);
	case OC_DONE: return opt_footer(false);
	default: return fep_rect(0, 0, 0, 0);
	}
}
static bool options_target_enabled(int target) {
	if(target < 0 || target >= OT_COUNT) return false;
	int row = options_row(target);
	if(row == OC_MUSIC || row == OC_SOUND) {
		float value = row == OC_MUSIC ? music_value : sound_value;
		bool plus = target == OT_MUSIC_PLUS || target == OT_SOUND_PLUS;
		return plus ? value < 1.0F : value > 0.0F;
	}
	/* W1F: a remote server owns its difficulty; the local option is shown
	 * but cannot be changed from inside a remote session. */
	if(row == OC_DIFFICULTY && gstate.remote_session)
		return false;
	return controls[row].enabled;
}
static void options_adjust(int row, int direction) {
	float* value = row == OC_MUSIC ? &music_value : &sound_value;
	*value = (roundf(*value * 100.0F) + direction * 5) / 100.0F;
	if(*value < 0.0F) *value = 0.0F;
	if(*value > 1.0F) *value = 1.0F;
	save_failed = false;
	options_refresh_labels();
}

static bool options_commit(void) {
	int old_sound = config_read_int(&gstate.config_user, "audio.volume", 100);
	if(old_sound < 0)
		old_sound = 0;
	if(old_sound > 100)
		old_sound = 100;
	int new_sound = (int)(sound_value * 100.0F);

	/* music.volume mirrors audio.volume's own default/range -- Beta's
	 * options.musicVolume is a slider INDEPENDENT of soundVolume
	 * [740c583 SoundManager], so it is set/committed alongside it here as
	 * ONE transaction, not folded into the same key. */
	int old_music = config_read_int(&gstate.config_user, "music.volume", 100);
	if(old_music < 0)
		old_music = 0;
	if(old_music > 100)
		old_music = 100;
	int new_music = (int)(music_value * 100.0F);
	int old_split = config_read_int(&gstate.config_user,
									"experimental.local_splitscreen", 0) ? 1 : 0;
	int old_difficulty = config_read_difficulty(&gstate.config_user);

	/* The texture-pack transaction precedent: set -> apply -> save; on any
	 * failure restore BOTH old keys, re-apply them, and re-save. */
	bool applied = config_set_int(&gstate.config_user, "audio.volume", new_sound)
		&& audio_set_master_volume(new_sound)
		&& config_set_int(&gstate.config_user, "music.volume", new_music)
		&& audio_music_set_volume(new_music)
		/* split_config_value(), not split_value: while a live request is
		 * outstanding the row shows what it is ASKING for, and leaving the
		 * screen then must not write an ungranted state into the file. */
		&& config_set_int(&gstate.config_user, "experimental.local_splitscreen",
						  split_config_value())
		&& config_set_difficulty(&gstate.config_user, difficulty_value)
		&& config_save(&gstate.config_user, "config.json");
	if(applied) {
		/* The deferred acknowledged value (A8-M6) is now on disk. */
		split_key_dirty = false;
		return true;
	}
	(void)config_set_int(&gstate.config_user, "experimental.local_splitscreen",
						 old_split);
	if(split_row_state != SPLIT_ROW_PENDING)
		split_value = old_split;
	/* W1F: the file keeps the old difficulty, so the world returns to it. */
	(void)config_set_difficulty(&gstate.config_user, old_difficulty);
	difficulty_apply(old_difficulty);

	if(config_set_int(&gstate.config_user, "audio.volume", old_sound))
		(void)audio_set_master_volume(old_sound);
	if(config_set_int(&gstate.config_user, "music.volume", old_music))
		(void)audio_music_set_volume(old_music);
	(void)config_save(&gstate.config_user, "config.json");
	sound_value = (float)old_sound / 100.0F;
	music_value = (float)old_music / 100.0F;
	options_refresh_labels();
	return false;
}

/* R-A8 A8-M6, second half (S-A5): the ack's config write is DEFERRED to the
 * next menu commit, which closes the "blocking libfat SD write inside a
 * gameplay frame" hole but opens a smaller one -- a player who toggles the
 * switch in-world and then LEAVES without ever reopening Options would lose the
 * acknowledged value. This is the explicit flush for that: main.c calls it at
 * the world's teardown, the last moment the value can still be written and a
 * moment that is already off the gameplay path (the frame that clears
 * gstate.world_loaded has already switched to the title/load screen).
 *
 * It writes ONLY the split key, through the same config_set_int + config_save
 * pair options_commit uses -- not options_commit itself, because that routine
 * also commits the Options screen's audio/music slider statics, which are not
 * the live values when the screen is not open.
 *
 * Costs nothing when nothing is owed: with the key clean this is a bool test
 * and no file is touched, so the ordinary unload path is unchanged. Returns
 * false only when the write itself failed, leaving the key dirty so a later
 * menu commit still carries it. */
bool screen_options_split_flush(void) {
	if(!split_key_dirty)
		return true;
	if(!config_set_int(&gstate.config_user, "experimental.local_splitscreen",
					   split_config_value())
	   || !config_save(&gstate.config_user, "config.json"))
		return false;
	split_key_dirty = false;
	return true;
}

static void options_leave(void) {
	/* Second Back after a failed save: discard the draft and leave. The
	 * failed options_commit already restored and re-applied the old values;
	 * any further edit clears save_failed and re-arms the save-on-Back path. */
	if(save_failed) {
		screen_set(options_return);
		return;
	}
	if(options_commit())
		screen_set(options_return);
	else
		save_failed = true;
}

static void screen_options_reset(struct screen* s, int width, int height) {
	s->render_world = screen_options_from_pause();
	(void)width;
	(void)height;
	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;
	input_pointer_enable(true);
	selected = OC_MUSIC;
	focus = (struct gutil_menu_focus) {0};
	repeat = (struct gutil_menu_repeat) {0};
	pointer_available = false;
	pointer_target = -1;
	origin = GUTIL_MENU_ORIGIN_DEFAULT;
	save_failed = false;
	int volume = config_read_int(&gstate.config_user, "audio.volume", 100);
	if(volume < 0)
		volume = 0;
	if(volume > 100)
		volume = 100;
	sound_value = (float)volume / 100.0F;
	int music_volume = config_read_int(&gstate.config_user, "music.volume", 100);
	if(music_volume < 0)
		music_volume = 0;
	if(music_volume > 100)
		music_volume = 100;
	music_value = (float)music_volume / 100.0F;
	/* W1F: the config IS the option (GameSettings); main.c applied it to the
	 * server at boot and every change since went through difficulty_apply. */
	difficulty_value = config_read_difficulty(&gstate.config_user);
	/* Each screen shows the truth about its OWN scope (R-A8 A8-M4):
	 *  - a request in flight shows the value it is asking for;
	 *  - IN-WORLD there is a live switch to disagree with, so the row shows the
	 *    ACKNOWLEDGED value -- what input arming actually follows;
	 *  - with NO live world (title screen, and before main.c's boot seed) the
	 *    row shows the FILE, which is what the next world load will apply.
	 * The last case is a regression fix: this used to prefer split_applied
	 * whenever it had been seeded, and main.c seeds it once before the main
	 * loop, so the file was never read again -- a title-screen toggle wrote the
	 * key and was then silently reverted in the row on the next reopen.
	 * The row therefore reads the FILE in BOTH directions at the title
	 * (ON->OFF and OFF->ON) and the ACKNOWLEDGED switch in both directions
	 * in-world; case (j) and case (q) of tests/test_split_live_options.c pin
	 * each half.
	 * The one-session divergence this comment used to describe is CLOSED
	 * (R-A9 A9-M5): screen_options_split_world_seed re-reads
	 * experimental.local_splitscreen on every world generation and asks the
	 * server to match it, so a title-screen edit now reaches the NEXT WORLD
	 * LOAD of this process and not just the next launch. What remains -- by
	 * design, and the reason the title screen still shows the FILE -- is that
	 * the edit does not move the live switch of the world already open: there
	 * is no world open to move while !split_live_available(), and the seed
	 * runs at a generation boundary, not at a menu commit.
	 * A FAILED / NO_ANSWER status is deliberately NOT cleared here -- reopening
	 * Options is exactly when the player wants to read why it did not apply. */
	if(split_row_state == SPLIT_ROW_PENDING)
		split_value = split_pending_want ? 1 : 0;
	else if(split_applied_seeded && split_live_available())
		split_value = split_applied ? 1 : 0;
	else
		split_value = config_read_int(&gstate.config_user,
									  "experimental.local_splitscreen", 0) ? 1 : 0;
	options_refresh_labels();
	if(!options_target_enabled(selected)) selected = OT_MUSIC_PLUS;
}

static void screen_options_update(struct screen* s, float dt) {
	(void)s;
	if(split_off_rearm) {
		bool active = input_held(IB_GUI_UP) || input_held(IB_GUI_DOWN)
			|| input_held(IB_GUI_LEFT) || input_held(IB_GUI_RIGHT)
			|| input_held(IB_GUI_CLICK) || input_held(IB_GUI_CLICK_ALT)
			|| input_held(IB_HOME) || input_held(IB_INVENTORY)
			|| input_pressed(IB_GUI_UP) || input_pressed(IB_GUI_DOWN)
			|| input_pressed(IB_GUI_LEFT) || input_pressed(IB_GUI_RIGHT)
			|| input_pressed(IB_GUI_CLICK) || input_pressed(IB_GUI_CLICK_ALT)
			|| input_pressed(IB_HOME) || input_pressed(IB_INVENTORY);
		if(split_off_rearm_step(active))
			return;
	}
	if(split_off_modal_update())
		return;
	pointer_available = input_pointer(&pointer_x, &pointer_y, &pointer_angle);
	bool up = input_held(IB_GUI_UP) || input_pressed(IB_GUI_UP);
	bool down = input_held(IB_GUI_DOWN) || input_pressed(IB_GUI_DOWN);
	bool left = input_held(IB_GUI_LEFT) || input_pressed(IB_GUI_LEFT);
	bool right = input_held(IB_GUI_RIGHT) || input_pressed(IB_GUI_RIGHT);
	bool navigating = up || down || left || right;
	bool owner = gutil_menu_pointer_focus(&focus, pointer_available, pointer_x, pointer_y, navigating);
	pointer_target = -1;
	if(pointer_available) for(int i = 0; i < OT_COUNT; i++) {
		if(fep_rect_hit(options_target_rect(i), pointer_x, pointer_y)) {
			pointer_target = i;
			if(owner) { selected = i; origin = GUTIL_MENU_ORIGIN_POINTER; }
			break;
		}
	}
	int step = gutil_menu_repeat_step(&repeat, (up || down) ? down - up : 2 * (right - left), dt);
	if(navigating) origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	if(step) {
		int row = options_row(selected);
		if(step == 1 || step == -1) {
			int visual = options_visual_row(selected) + step;
			if(visual < 0) visual = 0;
			if(visual >= (int)(sizeof(visual_targets) / sizeof(visual_targets[0])))
				visual = (int)(sizeof(visual_targets) / sizeof(visual_targets[0])) - 1;
			selected = visual_targets[visual];
			row = options_row(selected);
			if((row == OC_MUSIC || row == OC_SOUND) && !options_target_enabled(selected))
				selected = row == OC_MUSIC ? OT_MUSIC_PLUS : OT_SOUND_PLUS;
		} else if(row == OC_MUSIC || row == OC_SOUND) {
			selected = step > 0 ? (row == OC_MUSIC ? OT_MUSIC_PLUS : OT_SOUND_PLUS) : row;
			options_adjust(row, step / 2);
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		} else if(row == OC_SPLIT
		   && (input_pressed(IB_GUI_LEFT) || input_pressed(IB_GUI_RIGHT))
		   && split_ui_set(step > 0)) {
			save_failed = false;
			audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		}
		else if(row == OC_VIDEO || row == OC_DIFFICULTY)
			/* W1F: Left/Right walk the two halves of the row; only a
			 * fresh activation changes the value (Beta's click cycle). A
			 * disabled (remote) Difficulty half is not a focus stop. */
			selected = step > 0 && options_target_enabled(OT_DIFFICULTY)
				? OT_DIFFICULTY : OC_VIDEO;
	}
	bool click = input_pressed(IB_GUI_CLICK);
	struct gutil_menu_decision d = gutil_menu_activate_decide((struct gutil_menu_activate_state) {
		.pressed = click, .navigated = navigating, .pointer_valid = pointer_available,
		.selection_origin = origin, .hovered = pointer_target,
		.hovered_enabled = options_target_enabled(pointer_target), .selected = selected,
		.selected_enabled = options_target_enabled(selected),
	});
	selected = d.selected;
	origin = d.selection_origin;
	if(d.action == GUTIL_MENU_ACTIVATE) {
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		int row = options_row(d.target);
		if(row == OC_MUSIC || row == OC_SOUND)
			options_adjust(row,
				d.target == OT_MUSIC_PLUS || d.target == OT_SOUND_PLUS ? 1 : -1);
		else if(row == OC_SPLIT) {
			int shown = split_row_state == SPLIT_ROW_PENDING ? split_pending_want : split_value;
			save_failed = false;
			split_ui_set(!shown);
		} else if(row == OC_DIFFICULTY) {
			/* GuiOptions.actionPerformed -> setOptionValue(DIFFICULTY, 1):
			 * difficulty = difficulty + 1 & 3 (GameSettings.java:125). */
			save_failed = false;
			difficulty_apply(difficulty_cycle(difficulty_value, 1));
			options_refresh_labels();
		} else if(row == OC_VIDEO) {
			if(options_commit()) screen_set(&screen_video_settings);
			else save_failed = true;
		} else if(row == OC_CONTROLS) {
			if(options_commit()) screen_set(&screen_controller_settings);
			else save_failed = true;
		} else if(row == OC_DONE) options_leave();
	}
	/* Every activation edge is consumed, including a remapped Close collision. */
	if(click) return;
	if(input_pressed(IB_GUI_CLICK_ALT) || input_pressed(IB_HOME) || input_pressed(IB_INVENTORY)) {
		audio_play_ui(SOUND_RANDOM_CLICK, 1.0F, 1.0F);
		options_leave();
	}
}

static void screen_options_render2D(struct screen* s, int width, int height) {
	(void)s;
	if(screen_options_from_pause()) fep_fill(fep_rect(0, 0, width, height), 0, 0, 0, 180);
	else if(!fep_backdrop(width, height, 90)) gutil_bg();
	opt_text(32, 22, 28, 576, "Options", false);
	struct fep_rect panel = opt_rect(32, 78, 576, 330);
	fep_paper(panel); fep_bevel(panel);
	for(int row = OC_MUSIC; row <= OC_SOUND; row++) {
		int y = row == OC_MUSIC ? 98 : 160;
		int plus = row == OC_MUSIC ? OT_MUSIC_PLUS : OT_SOUND_PLUS;
		float value = row == OC_MUSIC ? music_value : sound_value;
		char text[16]; snprintf(text, sizeof(text), "%d%%", (int)roundf(value * 100.0F));
		opt_text(60, y + 13, 20, 204, controls[row].name, true);
		struct fep_rect value_rect = opt_rect(404, y + 13, 108, 20);
		gutil_text_rgb(value_rect.x + (value_rect.w - gutil_font_width(text, opt_px(19))) / 2,
			value_rect.y, text, opt_px(19), false, 32, 32, 32);
		opt_button(options_target_rect(row), "-", options_target_enabled(row), selected == row);
		opt_button(options_target_rect(plus), "+", options_target_enabled(plus), selected == plus);
	}
	opt_button(options_target_rect(OC_VIDEO), "Display", true, selected == OC_VIDEO);
	{
		char difficulty_label[64];
		options_format_label(OPTIONS_DIFFICULTY, 0.0F, difficulty_label,
							 sizeof(difficulty_label));
		opt_button_fit(options_target_rect(OT_DIFFICULTY), difficulty_label,
					   options_target_enabled(OT_DIFFICULTY),
					   selected == OT_DIFFICULTY);
	}
	opt_button(options_target_rect(OT_CONTROLS), "Controller settings", true,
			   selected == OT_CONTROLS);
	opt_text(60, 351, 18, 350, "Local split-screen", true);
	const char* split_label = split_row_state == SPLIT_ROW_PENDING ? "..." : split_value ? "On" : "Off";
	opt_button(options_target_rect(OC_SPLIT), split_label, true, selected == OC_SPLIT);
	if(split_value && selected != OC_SPLIT)
		fep_outline(options_target_rect(OC_SPLIT), opt_px(2), 48, 48, 48, 255);
	/* Preserve readable helper text below the label. The first line ends
	 * before the toggle; the second lies below it inside the paper panel. */
	if(save_failed)
		opt_text(60, 390, 14, 520,
			"Could not save. Press Back again to leave without saving", true);
	else if(selected == OC_SPLIT || split_row_state != SPLIT_ROW_SETTLED || split_status[0]) {
		const char *line1, *line2;
		split_helper_lines(&line1, &line2);
		opt_text(60, 371, 14, 400, line1, true);
		opt_text(60, 390, 14, 520, line2, true);
	}
	struct fep_rect back = opt_footer(true);
	if(selected == OC_DONE) fep_outline(back, opt_px(3), 255, 255, 255, 255);
	split_off_modal_render(width, height);
	if(pointer_available) {
		gfx_bind_texture(&texture_pointer);
		gutil_texquad_rt_any(pointer_x, pointer_y, glm_rad(pointer_angle), 0, 0, 256, 256, opt_px(48), opt_px(48));
	}
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
bool screen_options_test_target_rect(int target, struct gutil_button_bounds* out) {
	struct fep_rect r = options_target_rect(target);
	if(out) *out = (struct gutil_button_bounds) {r.x, r.y, r.w, r.h};
	return r.w > 0 && r.h > 0;
}
int screen_options_test_focus(void) { return selected; }
int screen_options_test_difficulty(void) { return difficulty_value; }
const char* screen_options_test_difficulty_label(void) {
	static char row[64];
	options_format_label(OPTIONS_DIFFICULTY, 0.0F, row, sizeof(row));
	return row;
}
bool screen_options_test_target_enabled(int target) { return options_target_enabled(target); }
size_t screen_options_test_control_count(void) {
	return OPTIONS_COUNT;
}

bool screen_options_test_control(size_t idx, int* id, const char** name,
								 bool* slider, bool* enabled) {
	if(idx >= OPTIONS_COUNT)
		return false;
	if(id)
		*id = controls[idx].id;
	if(name)
		*name = controls[idx].name;
	if(slider)
		*slider = controls[idx].slider;
	if(enabled)
		*enabled = controls[idx].enabled;
	return true;
}

float screen_options_test_slider_value(float pointer_x, int x, int width) {
	return options_slider_value(pointer_x, x, width);
}

int screen_options_test_slider_thumb(int x, int width, float value) {
	return options_slider_thumb(x, width, value);
}

void screen_options_test_format_label(int id, float value, char* out,
								  size_t out_size) {
	options_format_label(id, value, out, out_size);
}

bool screen_options_test_split_set(int wanted) {
	return split_row_set(wanted);
}

const char* screen_options_test_split_row(void) {
	static char row[64];
	options_format_label(OPTIONS_SPLIT, 0.0F, row, sizeof(row));
	return row;
}

const char* screen_options_test_split_status(void) {
	return split_status;
}

/* The focused row's helper block, line 0 or line 1 -- the production selector
 * itself (S-A13), so the approved wording is asserted per state rather than
 * described. Declared by tests/test_split_live_options.c: this assignment owns
 * screen_options.c and its test, not screen.h. */
const char* screen_options_test_split_helper(int line) {
	const char* l1;
	const char* l2;
	split_helper_lines(&l1, &l2);
	return line == 0 ? l1 : l2;
}

int screen_options_test_split_config_value(void) {
	return split_config_value();
}

bool screen_options_test_split_key_dirty(void) {
	return split_key_dirty;
}

bool screen_options_test_split_off_open(void) {
	return split_off_open();
}

void screen_options_test_split_off_choose(bool turn_off,
									 struct client_session* guest) {
	if(split_off.phase != SCREEN_SPLIT_OFF_CONFIRM)
		return;
	if(turn_off)
		screen_options_split_off_confirm(guest);
	else
		screen_options_split_off_cancel();
}

bool screen_options_test_split_off_retry(struct client_session* guest) {
	return screen_options_split_off_retry(guest);
}

bool screen_options_test_split_off_rearm_step(bool active) {
	return split_off_rearm_step(active);
}

void screen_options_test_split_off_rearm_arm(void) {
	split_off_rearm = true;
}

uint32_t screen_options_test_split_off_request_seq(void) {
	return split_off.split_request_seq;
}

int screen_options_test_split_off_focus(void) {
	return split_off.selected;
}

/* The activation state the split-OFF modal's press would hand the shared
 * helper for a fresh A with no direction, sprite at `hovered` (-1 = none). */
bool screen_options_test_split_off_destructive_dialog(int hovered) {
	int saved = pointer_target;
	bool saved_ptr = pointer_available;
	pointer_target = hovered;
	pointer_available = hovered >= 0;
	int first = split_off.phase == SCREEN_SPLIT_OFF_CONFIRM ? 0 : 1;
	struct gutil_menu_activate_state in = split_off_activate_state(true, false,
		split_off_open(), first);
	pointer_target = saved;
	pointer_available = saved_ptr;
	return in.destructive_dialog;
}

bool screen_options_test_split_off_rect(int target,
								struct gutil_button_bounds* out) {
	struct fep_rect r = target == -1 ? split_off_panel()
		: target >= 0 && target <= 1 ? split_off_button(target)
		: fep_rect(0, 0, 0, 0);
	if(out) *out = (struct gutil_button_bounds) {r.x, r.y, r.w, r.h};
	return r.w > 0 && r.h > 0;
}

void screen_options_test_reopen(void) {
	/* reset() ignores width/height (see its (void) casts), so the nominal
	 * 640x480 menu geometry stands in for a gfx_width() a headless test has
	 * no window to answer. */
	screen_options_reset(&screen_options, 640, 480);
}
#endif

struct screen screen_options = {
	.reset = screen_options_reset,
	.update = screen_options_update,
	.render2D = screen_options_render2D,
	.render3D = NULL,
	.render_world = false,
};
