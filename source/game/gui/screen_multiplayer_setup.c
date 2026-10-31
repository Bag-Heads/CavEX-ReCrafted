/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* NEW multiplayer setup screen (maintainer-requested, 2026-08-31), slices
 * v1 (name field + Quick Connect), v2 (saved server list: add/edit/
 * delete), and v3 (release-era GuiMultiplayer look: two-line rows,
 * per-row live ping/latency bars, Join/Direct Connect/Refresh) landed
 * together.
 *
 * CARVE-OUT #2 FRAMING (Wii-controller playability, CLAUDE.md's "Beta 1.7.3
 * parity vs. Wii-controller playability" section): Beta 1.7.3's own
 * GuiMultiplayer is just ONE address text field -- the username comes from
 * the launcher's session object, never typed in-game [740c583
 * GuiMultiplayer]. A Wii has no launcher and no keyboard, so neither half
 * of that contract can be reused verbatim: this screen adds an in-game name
 * field (OSK-driven, the same carve-out widget_osk.h already uses for
 * world create/rename) and a small saved-server list so a player never has
 * to re-type a whole "host:port" on a Wiimote. This is BUILT ON TOP of the
 * faithful baseline, per the "Beta 1.7.3 first, then build on top" rule:
 * the actual connect handshake this hands off to
 * (screen_multiplayer_connect.c's mpc_begin_connect) is completely
 * unchanged, still reads exactly mp.server + mp.username off
 * gstate.config_user, exactly like the interim PC-only path did before this
 * screen existed [screen_multiplayer_connect.c:143-199]. Net-new UI, no
 * Beta mechanic gets shortcut ahead of parity work by this landing.
 *
 * v3 STYLING REFERENCE (maintainer-requested 2026-08-31, GENERAL KNOWLEDGE
 * ONLY -- not a decompile citation): the two-line name/MOTD row, the
 * right-aligned "n/max" player count, and the 5-bar latency indicator all
 * mirror the LATER (release-era, post-Beta) Minecraft "GuiMultiplayer"
 * server list, from ordinary developer familiarity with that later UI --
 * this project's decompile pack is Beta 1.7.3-only and has no such screen
 * to cite [see CLAUDE.md's "Beta 1.7.3 FIRST" rule: this is an ADDITION on
 * top of the faithful base, gated behind carve-out #2 exactly like v1/v2
 * above, never a substitute for any 1.7.3 mechanic]. The wire-level ping
 * this reads (0xFE probe / 0xFF+string16 "MOTD\247online\247max" reply) is
 * the later "legacy server list ping" protocol, also general knowledge, not
 * decompiled -- see mps_ping_parse_response below. The bars are drawn as
 * flat colour quads (gfx_texture(false) + gutil_texquad_col), never sourced
 * from icons.png: Beta 1.7.3's own icons.png predates this indicator, and a
 * later game's icon asset would be shipping IP this project does not own.
 *
 * LAN discovery: evaluated and DEFERRED. Beta 1.7.3 has none (LAN discovery
 * is a much later MC feature), and an IOS UDP subnet broadcast/scan is
 * meaningfully more Wii network-stack risk than this slice's actual ask.
 * Quick Connect (servers[0], always the first list entry, kept as its own
 * button per the maintainer's explicit preference) covers the "get back
 * into the same server fast" need without it.
 *
 * SCREEN FLOW: screen_title's Multiplayer button now opens THIS screen
 * (screen_title.c title_activate); it is the only path in -- the old
 * direct screen_multiplayer_connect route is gone. Selecting a server here
 * (Quick Connect, a saved-list row's Join Server, Direct Connect's one-off
 * address, or the D-pad-direct Join on the selected row) writes
 * mp.server/mp.username into gstate.config_user and hands off to the
 * UNCHANGED screen_multiplayer_connect via the screen.h pointer -- see
 * mps_activate below.
 *
 * LIVE PING (v3): mps_ping_tick() runs a tiny per-frame state machine,
 * called unconditionally from screen_mps_update() every frame regardless of
 * which subview is showing, so a sweep keeps progressing while the player
 * is, say, editing a different row. Rows are probed SEQUENTIALLY, one
 * in-flight socket at a time (the Wii IOS socket-budget rule) via a
 * disposable background thread -- reusing thread.h's `thread_spawn_
 * disposable` + refcounted handoff pattern VERBATIM from
 * server_remote_transport.c's own `remote_connector_thread` /
 * `remote_connect_handoff` (the proven, already-hardware-certified answer
 * to "net_socket_connect can block unboundedly in DNS/TCP and must never
 * run on the render thread" -- re-deriving a second non-blocking connect
 * state machine from scratch when this codebase already has one built and
 * proven would be exactly the kind of shortcut CLAUDE.md's "do not take the
 * easy way out" rule warns against). The worker calls the EXISTING
 * net_socket_connect/_send/_receive primitives (source/platform/pc or wii/
 * net_socket.c) completely unmodified -- those are already the "nonblocking
 * connect, retry-to-EISCONN on Wii / nonblocking-connect+select on PC"
 * machinery the brief asks for, just wrapped as one blocking-shaped call;
 * running that wrapped call off the render thread is what makes it
 * non-blocking FROM the render thread's point of view, without touching
 * shared platform socket code the real (gameplay) MP connect path also
 * depends on.
 *
 * DEVIATION -- screen-side timeout is an ABANDON, not a hard cancel: this
 * project's net_socket.h has no mid-connect cancel primitive (by design --
 * see net_socket_connect's own doc comment), so a row that blows its
 * ~2s-connect / ~1.5s-reply budget is handled exactly like
 * remote_connect_handoff's own "abandoned" connector: the screen marks the
 * handoff abandoned (the worker's eventual write is dropped) and moves on
 * to the next row immediately, while the worker keeps running in the
 * background to its own natural bound and closes its own socket. This can
 * transiently leave more than one socket alive on Wii across an abandon
 * boundary; it is the SAME accepted, already-documented leak shape
 * thread.h's own `thread_dispose(t, false)` comment describes for the real
 * connector ("bounded by user connect attempts"), not a new risk class.
 *
 * DEVIATION -- the "red X-ish" unreachable bars state (spec item 2) is
 * drawn as five DIM RED bars, not a literal X glyph: gutil_util.h's flat-
 * quad primitive (gutil_texquad_col) is axis-aligned only, and the rotated
 * primitive that exists (gutil_texquad_rt_any) is texture-only, not a
 * flat-colour draw -- there is no rotated-flat-quad primitive to draw a
 * true diagonal X with. Paired with the red "Can't reach server" line 2
 * text, the state still reads unambiguously; a literal X icon is not
 * pursued here rather than adding a new rendering primitive for one label.
 *
 * DEVIATION from the literal v1/v2 design brief, still true in v3 and
 * documented per its own "note any place you deviated, with the reason"
 * instruction: the brief said "click a [saved-list] row = connect".
 * Implemented instead as click = SELECT, double-click (or the Join Server
 * button) = activate -- exactly screen_select_world.c's own established
 * world-row contract (slotlist_click_at's SLOT_LIST_CLICK_SELECT/_ACTIVATE,
 * screen_select_world.c:1015-1041). Reasons: (1) UI consistency with the
 * ONLY other saved-list screen in this codebase; (2) a single mis-click on
 * the Wii's IR pointer (the controller-playability rule's whole premise is
 * that pointer precision is not free) would otherwise silently dial an
 * unintended server. Quick Connect keeps the brief's "one click, no
 * confirmation" shape for the pinned/likely-intended target; the saved
 * list gets the safer, already-proven interaction instead. Also following
 * screen_select_world.c's own documented precedent (screen.h's comment
 * above screen_sworld_create_button_center): the bottom action strip is
 * POINTER-ONLY by design; a D-pad-only GUI_CLICK activates Join Server on
 * the D-pad-selected row directly, mirroring SWORLD_BUTTON_SELECT's own
 * D-pad-direct precedent exactly. */

#include "../../platform/input_glyph.h"
#include "screen.h"
/* Above the gate on purpose, same reason screen_multiplayer_connect.c
 * states: server_remote.h defines CAVEWII_MP_CLIENT. */
#include "../../network/server_remote.h"

#ifdef CAVEWII_MP_CLIENT

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../config.h"
#include "../../graphics/gfx_settings.h"
#include "../../graphics/gui_util.h"
#include "../../network/mp_codec.h"
#include "../../platform/audio.h"
#include "../../platform/gfx.h"
#include "../../platform/input.h"
#include "../../platform/input_owner.h"
#include "../../platform/time.h"
#include "../game_state.h"
/* net_socket.h/thread.h ordered after game_state.h to match entity.h's own
 * convention of pulling <m-lib/m-dict.h> before ../platform/thread.h
 * [source/entity/entity.h:23,33]. */
#include "../../platform/net_socket.h"
#include "../../platform/thread.h"
#include "widget_osk.h"
#include "widget_slotlist.h"
#include "frontend_presentation.h"
#include "play_session.h"

#ifdef PLATFORM_WII
#include "../../platform/wii/net_bringup.h"
#endif

/* Beta login cap [740c583 Packet1Login.readPacketData], mirrored here so
 * the OSK field cannot even TYPE past it (mp_login_username_valid is still
 * called at save time -- belt-and-braces, same shape as every other
 * OSK-driven field in this codebase). */
#define MPS_USERNAME_MAX 16

#define MPS_FIELD_H 20

/* ---- MAIN view layout (logical units) ----------------------------------- */
#define MPS_TITLE_Y 8
#define MPS_NAME_LABEL_Y 20
#define MPS_NAME_ROW_Y 30
#define MPS_QUICK_ROW_Y 56
#define MPS_LIST_TOP 82
#define MPS_ROW_H 36 /* matches screen_select_world.c's world-row height */
#define MPS_ROW_CONTENT_W 216 /* widget_slotlist.c's own fixed row width
								* (list->width/2 - 108*s .. +108*s) */

/* ---- Edit-name subview layout (mirrors screen_rename_world.c) ---------- */
#define MPS_EN_TITLE_Y 8
#define MPS_EN_LABEL_Y 34
#define MPS_EN_FIELD_Y 44
#define MPS_EN_OSK_Y 84
#define MPS_EN_BTN_Y 190

/* ---- Add/Edit-server subview layout (mirrors screen_create_world.c) ---- */
#define MPS_ES_TITLE_Y 8
#define MPS_ES_NAME_LABEL_Y 24
#define MPS_ES_NAME_FIELD_Y 34
#define MPS_ES_ADDR_LABEL_Y 58
#define MPS_ES_ADDR_FIELD_Y 68
#define MPS_ES_OSK_Y 96
#define MPS_ES_BTN_Y 190

enum mps_view {
	MPS_VIEW_MAIN = 0,
	MPS_VIEW_EDIT_NAME,
	MPS_VIEW_EDIT_SERVER,
	MPS_VIEW_DIRECT_CONNECT,
};

/* Two-row classic layout: row 1 Join/Direct Connect/Add, row 2
 * Edit/Delete/Refresh/Back. Order matches the enum so mps_button_rect's
 * `button < 3` split lines up with the two physical rows. */
enum mps_button {
	MPS_BUTTON_JOIN = 0,
	MPS_BUTTON_DIRECT,
	MPS_BUTTON_ADD,
	MPS_BUTTON_EDIT,
	MPS_BUTTON_DELETE,
	MPS_BUTTON_REFRESH,
	MPS_BUTTON_BACK,
	MPS_BUTTON_COUNT,
};

static enum mps_view view;
/* Options survives its child editor, but never allows input through it. */
static bool mps_options_open;
static bool mps_options_return;
static size_t mps_page;
static void mps_remember_selection(void);
static void mps_return_focus(void);

/* Saved-server list state. */
static struct config_mp_server servers[CONFIG_MP_SERVER_LIST_MAX];
static size_t server_count;
static size_t gui_selection;
static struct slotlist server_slots;
static size_t delete_index;

/* Player name (the "active-target" mp.username -- see config.h). */
static char username_buf[MPS_USERNAME_MAX + 4];
static char username_edit_buf[MPS_USERNAME_MAX + 4];
static bool name_edit_latch;

/* Add/Edit-server subview scratch. */
static char edit_name_buf[CONFIG_MP_SERVER_NAME_CAP + 4];
static char edit_addr_buf[CONFIG_MP_SERVER_ADDRESS_CAP + 4];
static int edit_focus; /* 0 = name, 1 = address */
static bool edit_is_new;
static size_t edit_index;
static bool edit_latch;

/* Direct Connect subview scratch (one address field, no name -- uses the
 * already-saved username_buf, never persisted to servers[]/config.json). */
static char direct_addr_buf[CONFIG_MP_SERVER_ADDRESS_CAP + 4];
static bool direct_latch;

static struct osk_frontend osk;
static bool field_editor_open;
static char field_snapshot[CONFIG_MP_SERVER_ADDRESS_CAP + CONFIG_MP_SERVER_NAME_CAP + 8];
static unsigned sub_previous_down, sub_blocked;
static bool sub_seen, sub_connected;
static int sub_category;
static struct gutil_menu_focus sub_pointer;
static bool pointer_available;
static float pointer_x, pointer_y, pointer_angle;
static float tick_accum;
static int button_hover; /* -1 none; view-dependent index otherwise */
static bool quick_hover;

/* ---- Pure Beta-adjacent helpers (test targets) -------------------------- */

/* Trims both fields [740c583 String.trim, shared via screen_cworld_java_trim]
 * and validates the address with the SAME parser mpc_begin_connect uses
 * [740c583 GuiMultiplayer, mp_codec.h] -- so a saved entry can never fail to
 * parse later than it could have been caught here. `out` may be NULL for a
 * preview-only validity check (the Save button's enable rule). */
static bool mps_validate_entry(const char* name_in, const char* addr_in,
							   struct config_mp_server* out) {
	char trimmed_name[CONFIG_MP_SERVER_NAME_CAP];
	char trimmed_addr[CONFIG_MP_SERVER_ADDRESS_CAP];
	screen_cworld_java_trim(name_in, trimmed_name, sizeof(trimmed_name));
	screen_cworld_java_trim(addr_in, trimmed_addr, sizeof(trimmed_addr));
	if(!trimmed_name[0])
		return false;

	char host_scratch[MP_ADDRESS_HOST_CAPACITY];
	uint16_t port_scratch;
	if(!mp_address_parse(trimmed_addr, host_scratch, sizeof(host_scratch),
						 &port_scratch)
	   || host_scratch[0] == '\0')
		return false;

	if(out) {
		snprintf(out->name, sizeof(out->name), "%s", trimmed_name);
		snprintf(out->address, sizeof(out->address), "%s", trimmed_addr);
	}
	return true;
}

/* Pure array mutation on the file-scope `servers[]` -- gstate/disk-free, the
 * unit-test target (screen_mps_test_add_server et al.). Production callers
 * (mps_try_save_server/mps_delete_confirmed) follow up with
 * mps_persist_servers() to reach config.json; tests never do. */
static bool mps_array_add(const char* name_in, const char* addr_in) {
	if(server_count >= CONFIG_MP_SERVER_LIST_MAX)
		return false;
	struct config_mp_server entry;
	if(!mps_validate_entry(name_in, addr_in, &entry))
		return false;
	servers[server_count++] = entry;
	return true;
}

static bool mps_array_edit(size_t idx, const char* name_in,
						   const char* addr_in) {
	if(idx >= server_count)
		return false;
	struct config_mp_server entry;
	if(!mps_validate_entry(name_in, addr_in, &entry))
		return false;
	servers[idx] = entry;
	return true;
}

static bool mps_array_delete(size_t idx) {
	if(idx >= server_count)
		return false;
	for(size_t i = idx; i + 1 < server_count; i++)
		servers[i] = servers[i + 1];
	server_count--;
	return true;
}

/* A CHOSEN name (mp_codec.h mp_username_chosen): Packet1's 16-code-unit
 * login maximum [740c583 Packet1Login.readPacketData], the Beta account
 * alphabet, and never the project's own "CaveWii" -- the same rule
 * mpc_begin_connect and the autoconnect path apply, so Save here and the
 * join gate below can never disagree. */
static bool mps_username_valid(const char* text) {
	return mp_username_chosen(text);
}

/* Join gate (maintainer rule 2026-09-11, docs/mp_chat_auth_research_2026-09-11.md
 * "Player name"): no join without a chosen name. Every join path -- Join
 * Server, Quick Connect, Direct Connect -- commits through mps_activate, so
 * that one function holds the gate: the target is parked here, the name
 * editor opens, and a successful Save resumes exactly that join. Cancel
 * (mps_return_to_main) abandons it. */
static struct config_mp_server pending_join;
static bool join_pending;
static void mps_enter_edit_name(void);
/* Defined with the sub-editor activation rule below. */
static void mps_sub_focus_reset(void);

/* Same selection-clamp shape as screen_select_world.c's sworld_next_selection
 * (screen_select_world.c:695-705); count==0 collapses to 0 rather than
 * underflowing count-1 on a size_t. */
static size_t mps_next_selection(size_t cur, size_t count, bool up,
								 bool down) {
	if(count == 0)
		return 0;
	if(up && cur > 0)
		cur--;
	if(down && cur + 1 < count)
		cur++;
	if(cur >= count)
		cur = count - 1;
	return cur;
}

/* Direct Connect's own validity gate: address only, same parser as every
 * other address field in this screen, no name required (uses username_buf). */
static bool mps_direct_addr_valid(void) {
	char trimmed[CONFIG_MP_SERVER_ADDRESS_CAP];
	screen_cworld_java_trim(direct_addr_buf, trimmed, sizeof(trimmed));
	char host_scratch[MP_ADDRESS_HOST_CAPACITY];
	uint16_t port_scratch;
	return trimmed[0]
		&& mp_address_parse(trimmed, host_scratch, sizeof(host_scratch),
							&port_scratch)
		&& host_scratch[0];
}

/* ---- Ping wire parsing + latency->bars mapping (pure, test targets) ----- */

/* MP_TEXT_CAPACITY + 1: large enough to hold the full decoded display
 * string (mps_ping_parse_response already bounds units < MP_TEXT_CAPACITY)
 * with room for the NUL, so the copy into motd[] can never truncate -- the
 * row draw itself only shows the leading ~27 columns regardless. */
#define MPS_PING_MOTD_CAP (MP_TEXT_CAPACITY + 1)

struct mps_ping_status {
	char motd[MPS_PING_MOTD_CAP];
	int32_t online; /* -1 = unknown/unparsed */
	int32_t max;	/* -1 = unknown/unparsed */
};

/* Overflow-safe non-negative decimal parse; anything non-numeric (including
 * empty) reports -1 (unknown) rather than garbage, and any value that would
 * overflow int32_t clamps to INT32_MAX instead of wrapping -- the "huge
 * values" hostile case. */
static int32_t mps_parse_ping_count(const char* s) {
	if(!s || !*s)
		return -1;
	int64_t v = 0;
	for(const char* p = s; *p; p++) {
		if(*p < '0' || *p > '9')
			return -1;
		v = v * 10 + (*p - '0');
		if(v > INT32_MAX)
			return INT32_MAX;
	}
	return (int32_t)v;
}

/* Parses a legacy server-list "ping" reply: Beta's own Packet255 Disconnect
 * shape (0xFF + string16) [740c583 Packet255KickDisconnect], repurposed by
 * later servers as an out-of-band status reply to a bare 0xFE probe byte --
 * GENERAL KNOWLEDGE (this file's header comment), not a b1.7.3 decompile
 * citation. Payload is "MOTD\247online\247max" (\247 = U+00A7, Beta's own
 * section-sign separator/colour-code byte, transcoded via the SAME single-
 * byte display charset gutil_text already renders
 * [mp_utf16_to_display, mp_codec.h]). `data`/`len` is whatever ONE recv()
 * call returned -- no re-framing/buffering across calls, matching the
 * "servers that say nothing within budget are bars-only" contract this
 * function's only caller (mps_ping_worker) implements.
 *
 * Returns false only when the frame is structurally unsafe to read
 * (wrong lead byte, a negative/oversized string16 length, or fewer bytes
 * present than the length promises -- the hostile/truncated cases). A
 * well-formed string with fewer than two \247 separators still returns
 * true with the WHOLE string as motd and online/max left -1: a real
 * legacy (pre-ping) Beta kick reply is exactly that shape, and showing it
 * beats discarding it. Splitting only on the FIRST two separators (not a
 * smarter delimiter-aware scan) deliberately mirrors that later client's
 * own historical shape: a MOTD containing a colour code mis-splits there
 * too. */
static bool mps_ping_parse_response(const uint8_t* data, size_t len,
									struct mps_ping_status* out) {
	if(!data || !out || len < 3 || data[0] != MP_PACKET_DISCONNECT)
		return false;

	int16_t raw_units
		= (int16_t)(((uint16_t)data[1] << 8) | (uint16_t)data[2]);
	if(raw_units < 0)
		return false; /* hostile negative length */
	size_t units = (size_t)raw_units;
	if(units >= MP_TEXT_CAPACITY)
		return false; /* would not fit mp_utf16's fixed buffer */
	if(len < 3 + units * 2)
		return false; /* truncated: fewer bytes than the length promises */

	struct mp_utf16 text;
	text.length = units;
	for(size_t i = 0; i < units; i++)
		text.units[i] = ((uint16_t)data[3 + i * 2] << 8)
			| (uint16_t)data[3 + i * 2 + 1];

	char display[MP_TEXT_CAPACITY + 1];
	if(!mp_utf16_to_display(&text, display, sizeof(display)))
		return false; /* malformed surrogate -- fail closed, matching the
					   * codec's own convention elsewhere */

	memset(out, 0, sizeof(*out));
	out->online = -1;
	out->max = -1;

	size_t sep1 = SIZE_MAX, sep2 = SIZE_MAX;
	size_t dlen = strlen(display);
	for(size_t i = 0; i < dlen; i++) {
		if((unsigned char)display[i] != 0xA7)
			continue;
		if(sep1 == SIZE_MAX)
			sep1 = i;
		else {
			sep2 = i;
			break;
		}
	}

	if(sep1 != SIZE_MAX && sep2 != SIZE_MAX) {
		display[sep1] = '\0';
		display[sep2] = '\0';
		snprintf(out->motd, sizeof(out->motd), "%s", display);
		out->online = mps_parse_ping_count(display + sep1 + 1);
		out->max = mps_parse_ping_count(display + sep2 + 1);
	} else {
		snprintf(out->motd, sizeof(out->motd), "%s", display);
	}
	return true;
}

/* Latency -> signal-bar count. These ms cuts are this screen's OWN choice
 * (general knowledge: release-era GuiMultiplayer buckets ping into 5 bars;
 * Beta 1.7.3 has no such UI to decompile a threshold from -- see header
 * comment). Spec: 5 bars <150ms, 4 <300, 3 <600, 2 <1000, 1 otherwise;
 * negative (not-yet-known) latency reports 0 (empty/gray, drawn separately
 * from the "reachable" state by the caller). */
static int mps_ping_bars_for_latency(int32_t latency_ms) {
	if(latency_ms < 0)
		return 0;
	if(latency_ms < 150)
		return 5;
	if(latency_ms < 300)
		return 4;
	if(latency_ms < 600)
		return 3;
	if(latency_ms < 1000)
		return 2;
	return 1;
}

/* ---- TOCTOU-safe ping-timeout revalidation (pure, test target) ---------
 * (audit finding 2, 2026-08-31.) mps_ping_tick's timeout branch snapshots
 * done/phase/phase_start under the lock, releases it, then decides "did the
 * deadline pass" -- but the worker can finish (or advance phase A -> B,
 * which grants its own fresh budget) in the gap before the lock is
 * reacquired. Blindly abandoning at that point discards a probe that may
 * have just SUCCEEDED and leaks a worker that has ALREADY finished (the
 * same needless-leak shape finding 1 fixes in mps_ping_reset_sweep).
 * mps_ping_tick reacquires the lock, re-reads done (and, if still false,
 * recomputes the deadline from the re-read phase/phase_start -- harmless
 * even when the phase did not change, and exactly what gives an advanced
 * phase its own fresh budget instead of phase A's already-expired one),
 * then asks this pure function what to do. Kept clock/lock-free so it is
 * headless-testable (screen_mps_test_ping_toctou_decide). */
enum mps_ping_toctou_action {
	MPS_PING_TOCTOU_WAIT = 0,
	MPS_PING_TOCTOU_CONSUME_DONE,
	MPS_PING_TOCTOU_ABANDON,
};

static enum mps_ping_toctou_action
mps_ping_toctou_decide(bool done_now, bool deadline_now_reached) {
	if(done_now)
		return MPS_PING_TOCTOU_CONSUME_DONE;
	return deadline_now_reached ? MPS_PING_TOCTOU_ABANDON
								: MPS_PING_TOCTOU_WAIT;
}

/* ---- Persistence glue (gstate/disk-touching; production only) ---------- */

/* Audit finding 3: sticky across mps_reset() ON PURPOSE (mps_reset() does
 * NOT clear it -- see that function's own comment) so a failed Join/Direct
 * Connect save is still visible after the round trip through
 * screen_multiplayer_connect and back ("shows the warning on return").
 * Cleared the moment any later save actually succeeds. */
static bool mps_save_failed;

static bool mps_persist_servers(void) {
	bool ok = config_write_server_list(&gstate.config_user, servers,
									   server_count)
		&& config_save(&gstate.config_user, "config.json");
	mps_save_failed = !ok;
	if(!ok)
		fprintf(stderr, "[mp-setup] failed to persist server list\n");
	return ok;
}

/* The scalar write-back (config.h's "active-target keys" contract): every
 * path that actually commits to connecting writes BOTH mp.server and
 * mp.username so mpc_begin_connect's unchanged read keeps working, then
 * hands off to the unchanged connect screen. */
static void mps_activate(const struct config_mp_server* srv) {
	mps_remember_selection();
	if(!mp_username_chosen(username_buf)) {
		pending_join = *srv;
		join_pending = true;
		mps_enter_edit_name();
		return;
	}
	/* Finding 4: every activation leaves this screen for
	 * screen_multiplayer_connect, so text capture must be off first --
	 * covers Direct Connect (the only OSK subview that reaches mps_activate
	 * without already having called mps_return_to_main()'s
	 * input_text_enable(false)). A harmless no-op-safe call when called from
	 * MAIN (Join/Quick Connect), where text capture is already off. */
	input_text_enable(false);

	bool ok = config_set_string(&gstate.config_user, "mp.server", srv->address)
		&& config_set_string(&gstate.config_user, "mp.username", username_buf)
		&& config_save(&gstate.config_user, "config.json");
	mps_save_failed = !ok;
	if(!ok)
		fprintf(stderr, "[mp-setup] failed to save mp.server/mp.username\n");
	/* Finding 3: connecting matters more than persistence -- proceed to the
	 * connect screen regardless of `ok`. gstate.config_user's in-memory
	 * scalars are already correct even when the on-disk write failed, so
	 * mpc_begin_connect (unchanged) still connects with the right values;
	 * mps_save_failed being sticky is what surfaces the warning "on
	 * return" (see its own comment). */
	if(screen_multiplayer_connect)
		screen_set(screen_multiplayer_connect);
	else
		fprintf(stderr,
				"[mp-setup] screen_multiplayer_connect not registered\n");
}

/* ---- Live ping subsystem (v3) -------------------------------------------- */

enum mps_ping_state {
	MPS_PING_IDLE = 0,	  /* not yet probed this screen visit */
	MPS_PING_PROBING,	  /* this row is the one currently in flight */
	MPS_PING_UNREACHABLE, /* connect failed, or the row's budget ran out */
	MPS_PING_REACHABLE,   /* connected; latency valid, status maybe unknown */
};

struct mps_row_ping {
	enum mps_ping_state state;
	int32_t latency_ms;
	bool has_status;
	struct mps_ping_status status;
};

static struct mps_row_ping row_ping[CONFIG_MP_SERVER_LIST_MAX];

#define MPS_PING_CONNECT_BUDGET_MS 2000
#define MPS_PING_PHASE_B_BUDGET_MS 1500

/* Disposable-connector handoff, deliberately the SAME shape as
 * server_remote_transport.c's remote_connect_handoff -- see this file's
 * header comment for why that pattern is reused rather than reinvented. */
struct mps_ping_handoff {
	struct thread_mutex lock;
	int refs; /* 2: the worker + the screen */
	bool abandoned;
	bool done;
	char host[MP_ADDRESS_HOST_CAPACITY];
	uint16_t port;
	int phase;			  /* 0 = TCP connect (phase A), 1 = awaiting the
						   * 0xFE reply (phase B) */
	ptime_t phase_start;
	bool reachable;
	int32_t latency_ms;
	bool has_status;
	struct mps_ping_status status;
};

static size_t ping_cursor;	  /* next servers[] index to probe */
static bool ping_in_flight;
static size_t ping_row_index; /* which row `ping_handoff` belongs to */
static struct thread ping_thread;
static struct mps_ping_handoff* ping_handoff;
static bool ping_net_ready; /* PC: always true; Wii: gated on net_bringup */

static void mps_ping_handoff_unref(struct mps_ping_handoff* h) {
	thread_mutex_lock(&h->lock);
	bool last = --h->refs == 0;
	thread_mutex_unlock(&h->lock);
	if(last) {
		thread_mutex_destroy(&h->lock);
		free(h);
	}
}

/* Runs entirely off the render thread. Reuses net_socket_connect/_send/
 * _receive completely UNMODIFIED (see header comment) -- a slow/silent
 * peer can hold THIS thread for as long as those calls' own internal
 * bounds allow; only the screen-side mps_ping_tick() enforces the tighter
 * ~2s/~1.5s per-row budget, by abandoning rather than blocking on it. */
static void* mps_ping_worker(void* user) {
	struct mps_ping_handoff* h = user;
	char host[MP_ADDRESS_HOST_CAPACITY];
	uint16_t port;
	thread_mutex_lock(&h->lock);
	snprintf(host, sizeof(host), "%s", h->host);
	port = h->port;
	thread_mutex_unlock(&h->lock);

	char err[64];
	bool session = net_socket_platform_init(err, sizeof(err));
	ptime_t connect_t0 = time_get();
	net_socket_t sock = session
		? net_socket_connect(host, port, err, sizeof(err))
		: NET_SOCKET_INVALID;
	bool reachable = sock != NET_SOCKET_INVALID;
	int32_t latency = reachable ? time_diff_ms(connect_t0, time_get()) : -1;

	bool has_status = false;
	struct mps_ping_status status;
	memset(&status, 0, sizeof(status));

	if(reachable) {
		bool abandoned_now;
		thread_mutex_lock(&h->lock);
		abandoned_now = h->abandoned;
		if(!abandoned_now) {
			h->reachable = true;
			h->latency_ms = latency;
			h->phase = 1;
			h->phase_start = time_get();
		}
		thread_mutex_unlock(&h->lock);

		if(!abandoned_now) {
			uint8_t probe = 0xFE;
			if(net_socket_send(sock, &probe, 1) == 1) {
				uint8_t buf[256];
				int n = net_socket_receive(sock, buf, sizeof(buf));
				if(n > 0)
					has_status = mps_ping_parse_response(buf, (size_t)n,
														 &status);
			}
		}
		net_socket_close(sock);
	}
	if(session)
		net_socket_platform_cleanup();

	thread_mutex_lock(&h->lock);
	if(!h->abandoned) {
		h->reachable = reachable;
		h->latency_ms = latency;
		h->has_status = has_status;
		h->status = status;
		h->done = true;
	}
	thread_mutex_unlock(&h->lock);
	mps_ping_handoff_unref(h);
	return NULL;
}

/* Abandons any in-flight probe and clears every row back to IDLE (gray,
 * "not yet pinged"). Called on screen entry, after a saved-list mutation,
 * and by the Refresh button. */
static void mps_ping_reset_sweep(void) {
	if(ping_in_flight) {
		bool done;
		thread_mutex_lock(&ping_handoff->lock);
		done = ping_handoff->done;
		if(!done)
			ping_handoff->abandoned = true;
		thread_mutex_unlock(&ping_handoff->lock);
		/* Audit finding 1b: a handoff whose worker already finished
		 * (done==true) is not genuinely "in flight" any more -- joining
		 * costs nothing (LWP_JoinThread/pthread_join return promptly, the
		 * worker is done or a few instructions from it) and avoids leaking
		 * the LWP bookkeeping + 64KB MEM2 stack for no reason. Only a
		 * worker that is STILL actually running takes the documented
		 * abandon-leak path: the worker keeps running to its own bound and
		 * closes its own socket; Wii keeps the LWP/stack on purpose
		 * (thread.h's documented disposable-connector leak). This is what
		 * bounds the leak to real timeouts instead of button-mashing
		 * Refresh or re-entering this screen mid-sweep (also see finding
		 * 1a's mps_button_enabled(MPS_BUTTON_REFRESH) gate below, which
		 * keeps Refresh from even reaching this call while a sweep is
		 * genuinely in flight). */
		thread_dispose(&ping_thread, done);
		mps_ping_handoff_unref(ping_handoff);
		ping_handoff = NULL;
		ping_in_flight = false;
	}
	for(size_t i = 0; i < CONFIG_MP_SERVER_LIST_MAX; i++) {
		row_ping[i].state = MPS_PING_IDLE;
		row_ping[i].latency_ms = -1;
		row_ping[i].has_status = false;
	}
	ping_cursor = 0;
}

static void mps_ping_start_row(size_t idx) {
	char host[MP_ADDRESS_HOST_CAPACITY];
	uint16_t port;
	if(!mp_address_parse(servers[idx].address, host, sizeof(host), &port)
	   || host[0] == '\0') {
		row_ping[idx].state = MPS_PING_UNREACHABLE;
		ping_cursor++;
		return;
	}

	struct mps_ping_handoff* h = calloc(1, sizeof(*h));
	if(!h || !thread_mutex_init(&h->lock)) {
		free(h);
		row_ping[idx].state = MPS_PING_UNREACHABLE;
		ping_cursor++;
		return;
	}
	snprintf(h->host, sizeof(h->host), "%s", host);
	h->port = port;
	h->refs = 2;
	h->phase = 0;
	h->phase_start = time_get();

	/* Priority 32, same "mesher band" thread_spawn_disposable's other
	 * caller uses (server_remote_transport.c) -- background probe work
	 * never competes with render/server/mesher for the core. */
	if(!thread_spawn_disposable(&ping_thread, mps_ping_worker, h, 32)) {
		thread_mutex_destroy(&h->lock);
		free(h);
		row_ping[idx].state = MPS_PING_UNREACHABLE;
		ping_cursor++;
		return;
	}
	row_ping[idx].state = MPS_PING_PROBING;
	ping_handoff = h;
	ping_in_flight = true;
	ping_row_index = idx;
}

/* Shared tail of the "consume a finished handoff" path -- used both by the
 * ordinary done==true case below and by the TOCTOU-revalidated done case in
 * the timeout branch (finding 2). Joins (never abandons) because the worker
 * has already finished. */
static void mps_ping_apply_done(bool reachable, int32_t latency,
								bool has_status,
								const struct mps_ping_status* status) {
	thread_dispose(&ping_thread, true);
	mps_ping_handoff_unref(ping_handoff);
	ping_handoff = NULL;
	ping_in_flight = false;
	if(ping_row_index < server_count) {
		row_ping[ping_row_index].state
			= reachable ? MPS_PING_REACHABLE : MPS_PING_UNREACHABLE;
		row_ping[ping_row_index].latency_ms = latency;
		row_ping[ping_row_index].has_status = has_status;
		if(has_status)
			row_ping[ping_row_index].status = *status;
	}
	ping_cursor++;
}

/* Shared tail of the "abandon a still-live probe past its budget" path
 * (finding 2's revalidated timeout, and the only path that still hits the
 * documented Wii LWP/stack leak). Caller must already hold+release the lock
 * and have set ping_handoff->abandoned = true before calling this. */
static void mps_ping_apply_abandon(bool reachable, int32_t latency) {
	thread_dispose(&ping_thread, false);
	mps_ping_handoff_unref(ping_handoff);
	ping_handoff = NULL;
	ping_in_flight = false;
	if(ping_row_index < server_count) {
		/* Beta servers need not implement the later 0xFE status protocol.
		 * Its timeout cannot undo a completed TCP connection. */
		row_ping[ping_row_index].state = reachable ? MPS_PING_REACHABLE : MPS_PING_UNREACHABLE;
		row_ping[ping_row_index].latency_ms = reachable ? latency : -1;
		row_ping[ping_row_index].has_status = false;
	}
	ping_cursor++;
}

/* The tiny per-frame state machine (brief item 4): call once per frame,
 * from every view, so a sweep keeps progressing no matter what subview the
 * player is looking at. */
static void mps_ping_tick(void) {
	if(ping_in_flight) {
		bool done, reachable = false, has_status = false;
		int32_t latency = -1;
		struct mps_ping_status status;
		int phase;
		ptime_t phase_start;
		memset(&status, 0, sizeof(status));

		thread_mutex_lock(&ping_handoff->lock);
		done = ping_handoff->done;
		phase = ping_handoff->phase;
		phase_start = ping_handoff->phase_start;
		if(done) {
			reachable = ping_handoff->reachable;
			latency = ping_handoff->latency_ms;
			has_status = ping_handoff->has_status;
			status = ping_handoff->status;
		}
		thread_mutex_unlock(&ping_handoff->lock);

		if(done) {
			mps_ping_apply_done(reachable, latency, has_status, &status);
			return;
		}

		unsigned budget_ms = phase == 0 ? MPS_PING_CONNECT_BUDGET_MS
										: MPS_PING_PHASE_B_BUDGET_MS;
		ptime_t deadline = time_add_ms(phase_start, budget_ms);
		if(time_reached(time_get(), deadline)) {
			/* Finding 2 (TOCTOU): re-lock and re-read before acting -- the
			 * worker may have finished, or advanced phase A -> B (its own
			 * fresh budget), in the gap between the snapshot above and this
			 * reacquire. */
			bool deadline_now_reached = false;
			thread_mutex_lock(&ping_handoff->lock);
			done = ping_handoff->done;
			if(done) {
				reachable = ping_handoff->reachable;
				latency = ping_handoff->latency_ms;
				has_status = ping_handoff->has_status;
				status = ping_handoff->status;
			} else {
				reachable = ping_handoff->reachable;
				latency = ping_handoff->latency_ms;
				phase = ping_handoff->phase;
				phase_start = ping_handoff->phase_start;
				budget_ms = phase == 0 ? MPS_PING_CONNECT_BUDGET_MS
										: MPS_PING_PHASE_B_BUDGET_MS;
				deadline = time_add_ms(phase_start, budget_ms);
				deadline_now_reached = time_reached(time_get(), deadline);
			}
			enum mps_ping_toctou_action action
				= mps_ping_toctou_decide(done, deadline_now_reached);
			if(action == MPS_PING_TOCTOU_ABANDON)
				ping_handoff->abandoned = true;
			thread_mutex_unlock(&ping_handoff->lock);

			if(action == MPS_PING_TOCTOU_CONSUME_DONE)
				mps_ping_apply_done(reachable, latency, has_status, &status);
			else if(action == MPS_PING_TOCTOU_ABANDON)
				mps_ping_apply_abandon(reachable, latency);
			/* MPS_PING_TOCTOU_WAIT: phase advanced and its recomputed
			 * deadline has not hit yet -- stay in flight, re-check next
			 * frame. */
		}
		return; /* one in-flight probe at a time */
	}

	if(!ping_net_ready || ping_cursor >= server_count)
		return;

	mps_ping_start_row(ping_cursor);
}

#ifdef PLATFORM_WII
/* Same readiness gate screen_multiplayer_connect.c's MPC_BRINGUP state
 * uses (net_bringup_status/_request) -- the console may have no IP yet at
 * boot (~25s DHCP settle), and net_bringup_request is documented
 * idempotent, so calling it here alongside the connect screen's own call
 * is safe. A FAILED verdict just leaves every row at MPS_PING_IDLE
 * (gray/unknown) forever -- Join Server/Quick Connect/Direct Connect all
 * still work without a live ping; the real connect screen is what
 * surfaces an actual network error if the player tries to connect. */
static void mps_ping_poll_bringup(void) {
	if(ping_net_ready)
		return;
	enum net_bringup_state st = net_bringup_status(NULL, 0);
	if(st == NET_BRINGUP_READY)
		ping_net_ready = true;
	else if(st == NET_BRINGUP_NOT_STARTED)
		net_bringup_request();
}
#endif

/* ---- View transitions ---------------------------------------------------- */

static void mps_return_to_main(void) {
	input_text_enable(false);
	field_editor_open = false;
	join_pending = false; /* a cancelled name edit abandons the parked join */
	view = MPS_VIEW_MAIN;
	mps_return_focus();
}

static void mps_edit_server_focus(int field) {
	edit_focus = field;
	osk_frontend_bind(&osk, field ? edit_addr_buf : edit_name_buf,
		field ? sizeof(edit_addr_buf) : sizeof(edit_name_buf),
		field ? MP_ADDRESS_INPUT_MAX : CONFIG_MP_SERVER_NAME_CAP - 1);
	osk.address = field != 0;
}

static void mps_enter_edit_name(void) {
	input_text_enable(false);
	snprintf(username_edit_buf, sizeof(username_edit_buf), "%s",
			username_buf);
	osk_frontend_reset(&osk, username_edit_buf, sizeof(username_edit_buf),
		MPS_USERNAME_MAX, 0, false);
	name_edit_latch = false;
	tick_accum = 0.0f;
	button_hover = -1;
	mps_sub_focus_reset();
	view = MPS_VIEW_EDIT_NAME;
}

/* Parent actions and editable fields share one canonical target space. */
enum { SUB_TARGET_COMMIT=1, SUB_TARGET_CANCEL=2,
	SUB_TARGET_FIELD_A=3, SUB_TARGET_FIELD_B=4 };
static int sub_focus=SUB_TARGET_CANCEL;
static enum gutil_menu_origin sub_origin;
static void mps_sub_focus_reset(void) {
	sub_focus=SUB_TARGET_CANCEL;
	sub_origin=GUTIL_MENU_ORIGIN_POINTER;
	sub_pointer=(struct gutil_menu_focus){0};
	sub_previous_down=sub_blocked=0;
	sub_seen=sub_connected=false;
	field_editor_open=false;
}

static void mps_enter_edit_server(bool is_new, size_t idx) {
	input_text_enable(false);
	edit_is_new = is_new;
	edit_index = idx;
	if(is_new) {
		edit_name_buf[0] = 0;
		edit_addr_buf[0] = 0;
	} else {
		snprintf(edit_name_buf, sizeof(edit_name_buf), "%s",
				servers[idx].name);
		snprintf(edit_addr_buf, sizeof(edit_addr_buf), "%s",
				servers[idx].address);
	}
	osk_frontend_reset(&osk, edit_name_buf, sizeof(edit_name_buf),
		CONFIG_MP_SERVER_NAME_CAP - 1, 0, false);
	mps_edit_server_focus(0);
	edit_latch = false;
	tick_accum = 0.0f;
	button_hover = -1;
	mps_sub_focus_reset();
	view = MPS_VIEW_EDIT_SERVER;
}

static void mps_enter_direct_connect(void) {
	input_text_enable(false);
	direct_addr_buf[0] = 0;
	osk_frontend_reset(&osk, direct_addr_buf, sizeof(direct_addr_buf),
		MP_ADDRESS_INPUT_MAX, 0, true);
	direct_latch = false;
	tick_accum = 0.0f;
	button_hover = -1;
	mps_sub_focus_reset();
	view = MPS_VIEW_DIRECT_CONNECT;
}

static void mps_try_save_name(void) {
	if(name_edit_latch || !mps_username_valid(username_edit_buf))
		return;
	name_edit_latch = true;
	snprintf(username_buf, sizeof(username_buf), "%s", username_edit_buf);
	/* screen_options.c's config_set_string + config_save pattern
	 * (screen_options.c:222-224). */
	bool ok = config_set_string(&gstate.config_user, "mp.username",
								username_buf)
		&& config_save(&gstate.config_user, "config.json");
	if(ok) {
		if(join_pending) {
			/* The join that opened this editor resumes with the new name. */
			struct config_mp_server target = pending_join;
			mps_return_to_main();
			mps_activate(&target);
			return;
		}
		mps_return_to_main();
	} else {
		fprintf(stderr, "[mp-setup] failed to save mp.username\n");
		name_edit_latch = false; /* stay, retryable */
	}
}

static void mps_try_save_server(void) {
	if(edit_latch || !mps_validate_entry(edit_name_buf, edit_addr_buf, NULL))
		return;
	edit_latch = true;
	bool ok = edit_is_new ? mps_array_add(edit_name_buf, edit_addr_buf)
						  : mps_array_edit(edit_index, edit_name_buf,
										   edit_addr_buf);
	if(ok) {
		/* Return value deliberately unused here: mps_persist_servers()
		 * already updates the sticky mps_save_failed flag (audit finding
		 * 3), and the in-memory servers[] mutation above already succeeded
		 * either way, so this session's list stays correct even if the
		 * disk write failed -- the player just gets warned about it (see
		 * mps_render_main). */
		(void)mps_persist_servers();
		gui_selection = edit_is_new ? server_count - 1 : edit_index;
	mps_remember_selection();
	mps_page = gui_selection / 4;
		slotlist_set_count(&server_slots, server_count);
		slotlist_select(&server_slots, gui_selection, true);
		/* The list changed shape (a new row, or an existing row's address
		 * may have changed) -- restart the ping sweep rather than track
		 * per-row index validity through the mutation. */
		mps_ping_reset_sweep();
		mps_return_to_main();
	} else {
		fprintf(stderr, "[mp-setup] failed to %s server entry\n",
				edit_is_new ? "add" : "edit");
		edit_latch = false; /* stay, retryable */
	}
}

/* Direct Connect never saves to servers[]/config.json (brief item 3) --
 * only the active-target mp.server/mp.username scalars mpc_begin_connect
 * reads are written, via the SAME mps_activate every other connect path
 * uses, just with a transient (non-array) entry backing it. */
static void mps_try_direct_connect(void) {
	if(direct_latch || !mps_direct_addr_valid())
		return;
	direct_latch = true;
	char trimmed[CONFIG_MP_SERVER_ADDRESS_CAP];
	screen_cworld_java_trim(direct_addr_buf, trimmed, sizeof(trimmed));
	struct config_mp_server transient;
	memset(&transient, 0, sizeof(transient));
	snprintf(transient.address, sizeof(transient.address), "%s", trimmed);
	mps_activate(&transient);
}

/* The confirm callback [screen_confirm.h contract, mirrors
 * screen_select_world.c:444-453's sworld_delete_confirmed]: re-entering this
 * screen re-reads the persisted list, so no manual slotlist/selection
 * fixup is needed here. */
static void mps_delete_confirmed(bool yes) {
	mps_options_return = !yes;
	if(yes && delete_index < server_count) {
		mps_array_delete(delete_index);
		/* See mps_try_save_server's identical comment: mps_save_failed
		 * already tracks this. */
		(void)mps_persist_servers();
	}
	screen_set(&screen_multiplayer_setup_impl);
}

static void mps_delete_selected(void) {
	mps_remember_selection();
	if(gui_selection >= server_count)
		return;
	delete_index = gui_selection;
	char warn[192];
	snprintf(warn, sizeof(warn),
			"'%s' (%s) will be removed from your saved servers.",
			servers[gui_selection].name, servers[gui_selection].address);
	/* screen_select_world.c:791-804's sworld_delete_selected pattern. */
	/* YES removes the row and persists the list, so Cancel is the answer that
	 * changes nothing. */
	screen_confirm_setup("Remove this saved server?", warn, "Remove",
						 "Cancel", SCREEN_CONFIRM_ANSWER_NO,
						 mps_delete_confirmed);
	screen_set(&screen_confirm);
}

/* Approved Join tab. Network, persistence and OSK transactions above remain
 * authoritative; only this presentation/navigation layer owns these targets. */
enum { MPS_FOCUS_NONE = -4, MPS_FOCUS_NAME = -3,
       MPS_FOCUS_QUICK = -2, MPS_FOCUS_LIST = -1 };
enum { MPS_TARGET_NAME = MPS_BUTTON_COUNT, MPS_TARGET_QUICK,
       MPS_TARGET_OPTIONS, MPS_TARGET_TAB_LOAD, MPS_TARGET_TAB_CREATE,
       MPS_TARGET_TAB_JOIN, MPS_TARGET_PREVIOUS, MPS_TARGET_NEXT,
       MPS_TARGET_ROW_BASE };
#define MPS_TARGET_ROW(i) (MPS_TARGET_ROW_BASE + (int)(i))
static int mps_focus = MPS_TARGET_TAB_JOIN;
static struct gutil_menu_focus mps_pointer_focus;
static bool mps_pointer_owns;
static enum gutil_menu_origin mps_origin;
static struct gutil_menu_activate_state mps_predict;

static void mps_remember_selection(void) {
 if(gui_selection < server_count)
  (void)play_session_remember_server(servers[gui_selection].name,
                                   servers[gui_selection].address, gui_selection);
}
static void mps_return_focus(void) {
 mps_focus = mps_options_open ? MPS_BUTTON_BACK :
             server_count ? MPS_FOCUS_LIST : MPS_BUTTON_ADD;
 mps_page = gui_selection / 4;
 mps_origin = GUTIL_MENU_ORIGIN_DEFAULT;
 mps_pointer_focus = (struct gutil_menu_focus){0};
}
static bool mps_target_is_row(int target) {
 return target >= MPS_TARGET_ROW_BASE;
}
static bool mps_button_enabled(int button) {
 switch(button) {
 case MPS_BUTTON_JOIN: {
  if(gui_selection >= server_count) return false;
  char host[MP_ADDRESS_HOST_CAPACITY]; uint16_t port;
  return mp_address_parse(servers[gui_selection].address, host, sizeof(host), &port);
 }
 case MPS_BUTTON_EDIT: case MPS_BUTTON_DELETE:
  return gui_selection < server_count;
 case MPS_BUTTON_DIRECT: case MPS_BUTTON_BACK: return true;
 case MPS_BUTTON_ADD: return server_count < CONFIG_MP_SERVER_LIST_MAX;
 case MPS_BUTTON_REFRESH: return server_count > 0 && ping_cursor >= server_count;
 default: return false;
 }
}
/* Physical bounds are derived once for rendering and hit testing. */
static struct fep_rect mps_rect(int x,int y,int w,int h) {
 int height=gfx_height(), scale=height;
 return fep_rect((gfx_width()-640*scale/480)/2+x*scale/480,
                 y*scale/480,w*scale/480,h*scale/480);
}
static int mps_px(int n) { return n*gfx_height()/480; }
static int mps_details_bottom(void) {
 return gui_selection < server_count && strlen(servers[gui_selection].address)>60 ? 360 : 324;
}
/* The centred hint row (controller UI redesign 10-31, A1 helper). Back's hit
 * target is the row's own Back cell, so the pointer hits what is drawn. */
static struct fep_rect mps_footer_row(bool draw) {
 struct input_player_frame prompts;input_prompt_frame(0,&prompts);
 static const struct gutil_hint_item items[]={
  {{IB_GUI_UP},1,"Move",1,false},{{IB_GUI_CLICK},1,"Select",2,false},
  {{IB_INVENTORY},1,"Back",0,true}};
 struct gutil_hint_row row;
 (void)gutil_hint_row_layout(&prompts,0,0,gfx_width(),gfx_height(),false,
  GUTIL_HINT_ALIGN_CENTRE,items,3,&row);
 struct fep_rect back=fep_rect(0,0,0,0);
 for(int k=0;k<row.count;k++) if(row.cells[k].item==2)
  back=fep_rect(row.cells[k].rect.x,row.cells[k].rect.y,row.cells[k].rect.width,row.cells[k].rect.height);
 if(draw) gutil_hint_row_draw(&row);
 return back;
}
static struct fep_rect mps_footer_back(void) { return mps_footer_row(false); }
static struct fep_rect mps_target_rect(int target) {
 if(mps_target_is_row(target)) {
  if(mps_options_open) return fep_rect(0,0,0,0);
  size_t i=(size_t)(target-MPS_TARGET_ROW_BASE);
  if(i/4 != mps_page || i>=server_count) return fep_rect(0,0,0,0);
  return mps_rect(100,110+(int)(i%4)*48,440,46);
 }
 if(mps_options_open) {
  int bottom=mps_details_bottom();
  switch(target) {
  case MPS_BUTTON_EDIT: return mps_rect(104,bottom-56,208,44);
  case MPS_BUTTON_DELETE: return mps_rect(328,bottom-56,208,44);
  case MPS_BUTTON_REFRESH: return mps_rect(104,bottom,208,44);
  case MPS_BUTTON_BACK: return mps_rect(328,bottom,208,44);
  default: return fep_rect(0,0,0,0);
  }
 }
 if(target>=MPS_TARGET_TAB_LOAD && target<=MPS_TARGET_TAB_JOIN)
  return mps_rect(88+156*(target-MPS_TARGET_TAB_LOAD),50,152,46);
 switch(target) {
 case MPS_TARGET_NAME: return mps_rect(326,4,226,44);
 case MPS_TARGET_PREVIOUS: return server_count>4?mps_rect(100,310,44,44):fep_rect(0,0,0,0);
 case MPS_TARGET_NEXT: return server_count>4?mps_rect(208,310,44,44):fep_rect(0,0,0,0);
 case MPS_BUTTON_JOIN: return server_count?mps_rect(100,366,276,44):fep_rect(0,0,0,0);
 case MPS_TARGET_OPTIONS: return server_count?mps_rect(392,366,148,44):fep_rect(0,0,0,0);
 case MPS_BUTTON_ADD: return server_count?mps_rect(264,310,128,44):mps_rect(100,242,276,44);
 case MPS_BUTTON_DIRECT: return server_count?mps_rect(396,310,144,44):mps_rect(392,242,148,44);
 case MPS_BUTTON_BACK: return mps_footer_back();
 default: return fep_rect(0,0,0,0);
 }
}
static bool mps_target_enabled(int target) {
 if(target==GUTIL_MENU_TARGET_NONE) return false;
 if(mps_options_open && target!=MPS_BUTTON_EDIT && target!=MPS_BUTTON_DELETE
    && target!=MPS_BUTTON_REFRESH && target!=MPS_BUTTON_BACK) return false;
 if(mps_target_is_row(target)) return (size_t)(target-MPS_TARGET_ROW_BASE)<server_count;
 switch(target) {
 case MPS_TARGET_NAME: case MPS_TARGET_TAB_LOAD: case MPS_TARGET_TAB_CREATE:
 case MPS_TARGET_TAB_JOIN: return true;
 case MPS_TARGET_OPTIONS: return gui_selection<server_count;
 case MPS_TARGET_PREVIOUS: return mps_page>0;
 case MPS_TARGET_NEXT: return (mps_page+1)*4<server_count;
 default: return mps_button_enabled(target);
 }
}
static int mps_focus_target(void) {
 if(mps_focus==MPS_FOCUS_LIST)
  return gui_selection<server_count?MPS_TARGET_ROW(gui_selection):GUTIL_MENU_TARGET_NONE;
 if(mps_focus==MPS_FOCUS_NAME) return MPS_TARGET_NAME;
 return mps_focus>=0?mps_focus:GUTIL_MENU_TARGET_NONE;
}
static void mps_focus_set_target(int target) {
 if(mps_target_is_row(target)) {
  gui_selection=(size_t)(target-MPS_TARGET_ROW_BASE); mps_focus=MPS_FOCUS_LIST;
  mps_page=gui_selection/4; slotlist_select(&server_slots,gui_selection,true);
  mps_remember_selection();
 } else mps_focus=target;
}
static void mps_focus_revalidate(void) {
 if(!mps_target_enabled(mps_focus_target())) mps_focus=MPS_FOCUS_NONE;
}
static const char* mps_nav_hint(void) { return NULL; }
static int mps_strip_top(int width,int height) {(void)width;(void)height;return mps_px(366);}
static void mps_page_change(int direction) {
 size_t pages=(server_count+3)/4,previous=mps_page;
 if(direction<0 && mps_page) mps_page--;
 else if(direction>0 && mps_page+1<pages) mps_page++;
 /* Paging never changes the selected server. It moves focus away from
  * an action on an offscreen selection until navigation reveals it again. */
 if(previous!=mps_page)mps_focus=direction<0?MPS_TARGET_PREVIOUS:MPS_TARGET_NEXT;
}
static void mps_activate_button(int button) {
 if(!mps_button_enabled(button)) return;
 audio_play_ui(SOUND_RANDOM_CLICK,1.0F,1.0F);
 mps_remember_selection();
 switch(button) {
 case MPS_BUTTON_JOIN: mps_page=gui_selection/4;mps_activate(&servers[gui_selection]);break;
 case MPS_BUTTON_DIRECT: mps_enter_direct_connect();break;
 case MPS_BUTTON_ADD: mps_enter_edit_server(true,0);break;
 case MPS_BUTTON_EDIT: mps_enter_edit_server(false,gui_selection);break;
 case MPS_BUTTON_DELETE: mps_delete_selected();break;
 case MPS_BUTTON_REFRESH: mps_ping_reset_sweep();mps_focus=MPS_FOCUS_NONE;break;
 case MPS_BUTTON_BACK:
  if(mps_options_open){mps_options_open=false;mps_focus=MPS_TARGET_OPTIONS;}
  else screen_set(&screen_title);
  break;
 default:break;
 }
}
static void mps_activate_target(int target) {
 if(mps_target_is_row(target)) {
  mps_focus_set_target(target);mps_focus=MPS_BUTTON_JOIN;
  audio_play_ui(SOUND_RANDOM_CLICK,1.0F,1.0F);return;
 }
 if(target>=MPS_TARGET_TAB_LOAD && target<=MPS_TARGET_TAB_JOIN) {
  enum play_tab tab=(enum play_tab)(target-MPS_TARGET_TAB_LOAD);
  mps_remember_selection();play_session_set_tab(tab);
  if(tab==PLAY_TAB_LOAD) screen_set(&screen_select_world);
  else if(tab==PLAY_TAB_CREATE) screen_set(&screen_create_world);
  return;
 }
 switch(target) {
 case MPS_TARGET_NAME:mps_enter_edit_name();return;
 case MPS_TARGET_OPTIONS:mps_page=gui_selection/4;mps_remember_selection();mps_options_open=true;mps_focus=MPS_BUTTON_BACK;
  mps_origin=GUTIL_MENU_ORIGIN_DEFAULT;return;
 case MPS_TARGET_PREVIOUS:mps_page_change(-1);return;
 case MPS_TARGET_NEXT:mps_page_change(1);return;
 default:mps_activate_button(target);return;
 }
}
int screen_mps_focus_target(void) {
 int t=mps_focus_target();
 if(mps_target_is_row(t))return SCREEN_MPS_FOCUS_LIST;
 switch(t) {
 case MPS_TARGET_NAME:return SCREEN_MPS_FOCUS_NAME;
 case MPS_BUTTON_JOIN:return SCREEN_MPS_FOCUS_JOIN;
 case MPS_BUTTON_DIRECT:return SCREEN_MPS_FOCUS_DIRECT;
 case MPS_BUTTON_ADD:return SCREEN_MPS_FOCUS_ADD;
 case MPS_BUTTON_EDIT:return SCREEN_MPS_FOCUS_EDIT;
 case MPS_BUTTON_DELETE:return SCREEN_MPS_FOCUS_DELETE;
 case MPS_BUTTON_REFRESH:return SCREEN_MPS_FOCUS_REFRESH;
 case MPS_BUTTON_BACK:return SCREEN_MPS_FOCUS_BACK;
 case MPS_TARGET_OPTIONS:return SCREEN_MPS_FOCUS_OPTIONS;
 case MPS_TARGET_TAB_LOAD:return SCREEN_MPS_FOCUS_TAB_LOAD;
 case MPS_TARGET_TAB_CREATE:return SCREEN_MPS_FOCUS_TAB_CREATE;
 case MPS_TARGET_TAB_JOIN:return SCREEN_MPS_FOCUS_TAB_JOIN;
 case MPS_TARGET_PREVIOUS:return SCREEN_MPS_FOCUS_PREV;
 case MPS_TARGET_NEXT:return SCREEN_MPS_FOCUS_NEXT;
 default:return SCREEN_MPS_FOCUS_NONE;
 }
}
int screen_mps_server_count(void) { return (int)server_count; }
int screen_mps_view(void) {
 switch(view) {
 case MPS_VIEW_EDIT_NAME:return SCREEN_MPS_VIEW_NAME;
 case MPS_VIEW_EDIT_SERVER:return edit_is_new?SCREEN_MPS_VIEW_ADD:SCREEN_MPS_VIEW_EDIT;
 case MPS_VIEW_DIRECT_CONNECT:return SCREEN_MPS_VIEW_DIRECT;
 default:return SCREEN_MPS_VIEW_MAIN;
 }
}
static void mps_reset(struct screen* s, int width, int height) {
	(void)s;
	if(gstate.players[0].local_player)
		gstate.players[0].local_player->data.local_player.capture_input = false;
	input_pointer_enable(true);
	input_text_enable(false);

	server_count = 0;
	config_read_server_list(&gstate.config_user, servers,
							CONFIG_MP_SERVER_LIST_MAX, &server_count);

	/* No default name: an unset or legacy-default ("CaveWii") name loads as
	 * EMPTY so the field reads "not chosen" and the join gate opens the
	 * editor with a blank line, not a prefilled name the player must delete. */
	const char* cfg_user
		= config_read_string(&gstate.config_user, "mp.username", "");
	snprintf(username_buf, sizeof(username_buf), "%s",
			 mp_username_chosen(cfg_user) ? cfg_user : "");
	join_pending = false;

	view = MPS_VIEW_MAIN;
	pointer_available = false;
	button_hover = -1;
	quick_hover = false;
	mps_focus = MPS_TARGET_TAB_JOIN;
	mps_options_open = mps_options_return;
	mps_options_return = false;
	play_session_set_tab(PLAY_TAB_JOIN);
	mps_pointer_focus = (struct gutil_menu_focus) {0};
	mps_pointer_owns = false;
	name_edit_latch = false;
	edit_latch = false;
	direct_latch = false;
	tick_accum = 0.0f;

	gui_selection = 0;
	size_t remembered = play_session_server_index();
	const char* rn = play_session_server_name();
	const char* ra = play_session_server_address();
	if(remembered < server_count && !strcmp(servers[remembered].name, rn)
	   && !strcmp(servers[remembered].address, ra))
		gui_selection = remembered;
	else for(size_t i = 0; i < server_count; i++)
		if(!strcmp(servers[i].name, rn) && !strcmp(servers[i].address, ra)) {
			gui_selection = i; break;
		}
	mps_page = gui_selection / 4;
	if(mps_options_open && !server_count) mps_options_open = false;
	if(mps_options_open) mps_focus = MPS_BUTTON_BACK;
	mps_origin = GUTIL_MENU_ORIGIN_DEFAULT;
	/* The observer's prediction is otherwise left over from the PREVIOUS time
	 * this screen ran and would answer for a ring that no longer exists. No
	 * pointer sample has been taken yet. */
	mps_predict = (struct gutil_menu_activate_state) {
		.pressed = true,
		.hovered = GUTIL_MENU_TARGET_NONE,
		.selection_origin = mps_origin,
		.selected = mps_focus_target(),
		.selected_enabled = mps_target_enabled(mps_focus_target()),
	};
	/* mps_save_failed is DELIBERATELY NOT reset here (audit finding 3): a
	 * Join/Direct Connect that failed to persist hands off to
	 * screen_multiplayer_connect immediately, so the warning can only ever
	 * be SEEN once the player returns to this screen -- and every return
	 * path (Back from the connect screen, a failed connect routing back
	 * here, or simply re-entering from the title) runs this reset(). Left
	 * sticky, it survives that round trip; the next successful save (or a
	 * fresh mps_persist_servers()/mps_activate() failure) is what updates
	 * it next. */
	/* [740c583 GuiWorldSlot.GuiWorldSlot]-shaped viewport (row height 36),
	 * pushed down from screen_select_world.c's top=32 to MPS_LIST_TOP so the
	 * name field + Quick Connect row above it fit. */
	slotlist_reset(&server_slots, width, MPS_LIST_TOP * GFX_GUI_SCALE,
				  height - 64 * GFX_GUI_SCALE, MPS_ROW_H * GFX_GUI_SCALE,
				  server_count, gui_selection);

	/* Live ping: fresh sweep every time this screen is entered (brief item
	 * 4, "when the screen opens"). PC has no bring-up step (WSAStartup is
	 * instant); Wii gates behind the same net_bringup readiness the connect
	 * screen uses -- polled from mps_ping_poll_bringup() each frame. */
	mps_ping_reset_sweep();
#ifdef PLATFORM_WII
	ping_net_ready = false;
#else
	ping_net_ready = true;
#endif
}


static int mps_pointer_target(void) {
 if(!pointer_available) return GUTIL_MENU_TARGET_NONE;
 for(int t=0;t<MPS_TARGET_ROW_BASE+(int)server_count;t++)
  if(fep_rect_hit(mps_target_rect(t),pointer_x,pointer_y))return t;
 return GUTIL_MENU_TARGET_NONE;
}
static void mps_navigate(bool up,bool down,bool left,bool right) {
 int t=mps_focus_target();
 if(mps_options_open) {
  static const int order[]={MPS_BUTTON_EDIT,MPS_BUTTON_DELETE,MPS_BUTTON_REFRESH,MPS_BUTTON_BACK};
  int at=-1;for(int i=0;i<4;i++)if(order[i]==t)at=i;
  int dir=(up||left)?-1:1;
  for(int i=0;i<4;i++){at=(at+dir+4)%4;if(mps_target_enabled(order[at])){mps_focus=order[at];break;}}
  return;
 }
 if(t>=MPS_TARGET_TAB_LOAD&&t<=MPS_TARGET_TAB_JOIN) {
  if(left||right)mps_focus=MPS_TARGET_TAB_LOAD+(t-MPS_TARGET_TAB_LOAD+(left?2:1))%3;
  else if(up)mps_focus=MPS_TARGET_NAME;
  else if(down){mps_focus=server_count?MPS_FOCUS_LIST:MPS_BUTTON_ADD;mps_page=gui_selection/4;}
  return;
 }
 if(t==MPS_TARGET_NAME){if(down)mps_focus=MPS_TARGET_TAB_JOIN;return;}
 if(mps_target_is_row(t)) {
  if(up && !gui_selection){mps_focus=MPS_TARGET_TAB_JOIN;return;}
  size_t next=mps_next_selection(gui_selection,server_count,up,down);
  if(next!=gui_selection)mps_focus_set_target(MPS_TARGET_ROW(next));
  else if(down)mps_focus=MPS_BUTTON_JOIN;
  if(right){mps_focus=MPS_BUTTON_JOIN;mps_page=gui_selection/4;}
  return;
 }
 if(up){mps_focus=server_count?MPS_FOCUS_LIST:MPS_TARGET_TAB_JOIN;mps_page=gui_selection/4;return;}
 static const int order[]={MPS_TARGET_PREVIOUS,MPS_TARGET_NEXT,MPS_BUTTON_ADD,MPS_BUTTON_DIRECT,MPS_BUTTON_JOIN,MPS_TARGET_OPTIONS,MPS_BUTTON_BACK};
 int at=-1;for(int i=0;i<7;i++)if(order[i]==t)at=i;
 int dir=left?-1:1;
 for(int i=0;i<7;i++){at=(at+dir+7)%7;struct fep_rect r=mps_target_rect(order[at]);if(r.w&&mps_target_enabled(order[at])){mps_focus=order[at];break;}}
}
static void mps_update_main(struct screen* s,float dt) {
 (void)s; tick_accum+=dt;
 pointer_available=input_pointer(&pointer_x,&pointer_y,&pointer_angle);
 bool up=input_pressed(IB_GUI_UP),down=input_pressed(IB_GUI_DOWN);
 bool left=input_pressed(IB_GUI_LEFT),right=input_pressed(IB_GUI_RIGHT);
 bool nav=up||down||left||right;
 mps_pointer_owns=gutil_menu_pointer_focus(&mps_pointer_focus,pointer_available,pointer_x,pointer_y,nav);
 mps_focus_revalidate();
 int hovered=mps_pointer_target();
 if(mps_pointer_owns&&!mps_target_is_row(hovered)&&mps_target_enabled(hovered)) {
  mps_focus_set_target(hovered);mps_origin=GUTIL_MENU_ORIGIN_POINTER;
 }
 if(nav){mps_origin=GUTIL_MENU_ORIGIN_DIRECTIONAL;mps_navigate(up,down,left,right);}
 bool click=input_pressed(IB_GUI_CLICK);
 struct gutil_menu_activate_state state={.pressed=click,.navigated=nav,
  .pointer_valid=pointer_available,.selection_origin=mps_origin,.hovered=hovered,
  .hovered_enabled=mps_target_enabled(hovered),.selected=mps_focus_target(),
  .selected_enabled=mps_target_enabled(mps_focus_target())};
 struct gutil_menu_decision d=gutil_menu_activate_decide(state);
 if(d.action==GUTIL_MENU_FOCUS_ONLY)mps_focus_set_target(d.target);
 mps_origin=d.selection_origin;
 if(d.action==GUTIL_MENU_ACTIVATE)mps_activate_target(d.target);
 state.pressed=true;state.navigated=nav;state.selection_origin=mps_origin;
 state.selected=mps_focus_target();state.selected_enabled=mps_target_enabled(state.selected);
 state.hovered_enabled=mps_target_enabled(state.hovered);
 mps_predict=state;
 if(click)return;
 if(!mps_options_open) {
  if(input_pressed(IB_GUI_PREVIOUS))mps_page_change(-1);
  if(input_pressed(IB_GUI_PAGE))mps_page_change(1);
 }
 if(input_pressed(IB_HOME)||input_pressed(IB_INVENTORY))mps_activate_button(MPS_BUTTON_BACK);
}
/* A row now SELECTS. Only the explicit Join action can satisfy the observer. */
bool screen_mps_focus_is_join(void) {
 if(gstate.players[0].current_screen!=&screen_multiplayer_setup_impl||view!=MPS_VIEW_MAIN||mps_options_open)return false;
 if(input_held(IB_GUI_CLICK)&&!input_pressed(IB_GUI_CLICK))return false;
 if(!screen_multiplayer_connect||!mp_username_chosen(username_buf))return false;
 struct gutil_menu_decision d=gutil_menu_activate_decide(mps_predict);
 return d.action==GUTIL_MENU_ACTIVATE&&d.target==MPS_BUTTON_JOIN&&mps_button_enabled(MPS_BUTTON_JOIN);
}
bool screen_load_focus_is_join(void) {
 return play_session_tab()==PLAY_TAB_JOIN&&screen_mps_focus_is_join();
}
static void mps_text(int x,int y,int size,int room,const char* text,bool dark) {
 char fit[512];fep_text_fit(fit,sizeof(fit),text,mps_px(size),mps_px(room));
 struct fep_rect r=mps_rect(x,y,0,0);
 gutil_text_rgb(r.x,r.y,fit,mps_px(size),!dark,dark?53:244,dark?53:244,dark?53:237);
}
static void mps_draw_control(int target,const char* label,bool quiet) {
 struct fep_rect r=mps_target_rect(target);if(!r.w)return;
 bool enabled=mps_target_enabled(target),focused=mps_focus_target()==target;
 if(!quiet||focused)fep_widget(r,focused&&enabled);else fep_fill(r,0,0,0,70);
 if(focused)fep_outline(r,2,255,255,160,255);
 if(!enabled)fep_fill(r,0,0,0,110);
 char fit[96];int size=mps_px(target==MPS_BUTTON_DIRECT||target==MPS_TARGET_OPTIONS?14:16);
 fep_text_fit(fit,sizeof(fit),label,size,r.w-mps_px(8));
 gutil_text_rgb(r.x+(r.w-gutil_font_width(fit,size))/2,r.y+(r.h-size)/2,fit,size,true,
               enabled?244:160,enabled?244:160,enabled?237:160);
}
/* Original pixel shapes; measured latency, unknown and failure are distinct.
 * The renderer does not turn a missing legacy status response into a success. */
static void mps_draw_ping(int x,int y,const struct mps_row_ping* p) {
 int frame=0;
 if(p->state==MPS_PING_UNREACHABLE)frame=1;
 else if(p->state==MPS_PING_PROBING)frame=7+(int)(tick_accum/0.4f)%2;
 else if(p->state==MPS_PING_REACHABLE&&p->latency_ms>=0)
  frame=mps_ping_bars_for_latency(p->latency_ms)+1;
 /* The approved sheet has nine 16px frames. UVs are normalized because
  * gutil_texquad's fixed 1/256 coordinates do not describe a144px sheet. */
 const struct tex_gfx* tex=&texture_menu_reception;
 if(tex->data&&tex->width>=144&&tex->height>=16) {
  struct fep_rect r=mps_rect(x,y,32,32);
  float u0=(float)(frame*16)/(float)tex->width;
  float u1=(float)(frame*16+16)/(float)tex->width;
  float v1=16.0F/(float)tex->height;
  gfx_bind_texture((struct tex_gfx*)tex);
  gfx_draw_quads_flt(4,
   (float[]){r.x,r.y,-2,r.x+r.w,r.y,-2,r.x+r.w,r.y+r.h,-2,r.x,r.y+r.h,-2},
   (uint8_t[]){255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255},
   (float[]){u0,0,u1,0,u1,v1,u0,v1});
  return;
 }

 if(p->state==MPS_PING_IDLE || (p->state==MPS_PING_REACHABLE&&p->latency_ms<0)) {
  fep_fill(mps_rect(x+10,y+15,12,2),180,180,175,255);return;
 }
 if(p->state==MPS_PING_UNREACHABLE) {
  for(int i=0;i<10;i++){fep_fill(mps_rect(x+6+i*2,y+6+i*2,2,2),190,90,75,255);fep_fill(mps_rect(x+24-i*2,y+6+i*2,2,2),190,90,75,255);}return;
 }
 if(p->state==MPS_PING_PROBING) {
  for(int i=0;i<2+(int)(tick_accum/0.4f)%2;i++)fep_fill(mps_rect(x+6+i*8,y+15,4,4),180,180,175,255);
  return;
 }
 int bars=mps_ping_bars_for_latency(p->latency_ms);
 for(int i=0;i<5;i++)fep_fill(mps_rect(x+2+i*6,y+28-(i+1)*5,4,(i+1)*5),i<bars?180:70,i<bars?180:70,i<bars?175:65,255);
}
static float menu_motion_time;

static void mps_render_main(int width,int height) {
 if(!fep_backdrop_at(width,height,90,menu_motion_time,true))gutil_bg();
 mps_text(88,14,26,230,"Play game",false);
 struct fep_rect panel=mps_rect(88,94,464,server_count?214:132);
 fep_paper(panel);fep_bevel(panel);
 static const char* tabs[]={"Load","Create","Join"};
 for(int i=0;i<3;i++) {
  struct fep_rect r=mps_rect(88+i*156,50,152,46);
  if(mps_focus_target()==MPS_TARGET_TAB_LOAD+i)fep_widget(r,true);else{fep_paper(r);if(i!=2)fep_fill(r,0,0,0,35);}
  fep_bevel(r);mps_text(88+i*156+32,63,19,116,tabs[i],mps_focus_target()!=MPS_TARGET_TAB_LOAD+i);
  if(i==2)fep_fill(mps_rect(88+i*156+12,88,128,4),53,53,53,255);
 }
 char label[128];snprintf(label,sizeof(label),"Player: %s",username_buf[0]?username_buf:"not chosen");
 mps_draw_control(MPS_TARGET_NAME,label,true);
 if(!server_count){mps_text(100,132,23,440,"No saved servers",true);mps_text(100,178,16,440,"Add a server or connect directly.",true);}
 for(size_t i=mps_page*4;i<server_count&&i<(mps_page+1)*4;i++) {
  struct fep_rect r=mps_target_rect(MPS_TARGET_ROW(i));bool focused=mps_focus_target()==MPS_TARGET_ROW(i);
  fep_widget_row(r,focused);fep_bevel(r);
  if(i==gui_selection)fep_outline(r,2,255,255,160,255);
  int y=110+(int)(i%4)*48;
  mps_draw_ping(108,y+7,&row_ping[i]);
  mps_text(152,y+2,19,378,servers[i].name,false);
  mps_text(152,y+26,13,378,servers[i].address,false);
 }
 if(server_count>4){mps_draw_control(MPS_TARGET_PREVIOUS,"<",true);mps_draw_control(MPS_TARGET_NEXT,">",true);snprintf(label,sizeof(label),"%zu / %zu",mps_page+1,(server_count+3)/4);mps_text(148,325,15,60,label,false);}
 else if(server_count){snprintf(label,sizeof(label),"%zu server%s",server_count,server_count==1?"":"s");mps_text(108,325,15,150,label,false);}
 mps_draw_control(MPS_BUTTON_ADD,"Add server",server_count>0);
 mps_draw_control(MPS_BUTTON_DIRECT,"Direct connect",true);
 mps_draw_control(MPS_BUTTON_JOIN,"Join server",false);
 mps_draw_control(MPS_TARGET_OPTIONS,"Server options",true);
 if(mps_save_failed)mps_text(100,412,13,440,"Couldn't save multiplayer settings",false);
 if(!mps_options_open || view!=MPS_VIEW_MAIN) {
  (void)mps_footer_row(true);
 } else if(view==MPS_VIEW_MAIN && gui_selection<server_count) {
  int bottom=mps_details_bottom(),top=bottom==360?76:112;
  fep_fill(fep_rect(0,0,width,height),0,0,0,160);panel=mps_rect(80,top,480,bottom-top+64);fep_paper(panel);fep_bevel(panel);
  mps_text(104,top+18,24,432,"Server options",true);
  mps_text(104,top+58,20,432,servers[gui_selection].name,true);
  /* Preserve the complete address with bounded character wrapping. */
  const char* at=servers[gui_selection].address;int yy=top+90;
  while(*at && yy<bottom-58){char line[CONFIG_MP_SERVER_ADDRESS_CAP];size_t n=0;
   while(at[n]&&n+1<sizeof(line)){line[n]=at[n];line[n+1]=0;if(gutil_font_width(line,mps_px(13))>mps_px(432)){line[n]=0;break;}n++;}
   if(!n)break;
   mps_text(104,yy,13,432,line,true);at+=n;yy+=18;
  }
  const struct mps_row_ping* p=&row_ping[gui_selection];
  if(p->state==MPS_PING_REACHABLE && p->latency_ms>=0) {
   if(p->has_status&&p->status.online>=0&&p->status.max>=0)
    snprintf(label,sizeof(label),"%d ms - %d / %d players",(int)p->latency_ms,(int)p->status.online,(int)p->status.max);
   else snprintf(label,sizeof(label),"%d ms - status unavailable",(int)p->latency_ms);
  }
  else snprintf(label,sizeof(label),"%s",p->state==MPS_PING_PROBING?"Checking connection":p->state==MPS_PING_UNREACHABLE?"No response":"Not checked");
  if(yy+14<bottom-56){mps_text(104,yy,13,432,label,true);yy+=18;}
  if(p->has_status&&p->status.motd[0]&&yy+14<bottom-56)
   mps_text(104,yy,13,432,p->status.motd,true);
  mps_draw_control(MPS_BUTTON_EDIT,"Edit server",false);mps_draw_control(MPS_BUTTON_DELETE,"Remove",false);
  mps_draw_control(MPS_BUTTON_REFRESH,"Refresh list",false);mps_draw_control(MPS_BUTTON_BACK,"Back",false);
 }
 if(pointer_available && view==MPS_VIEW_MAIN){gfx_bind_texture(&texture_pointer);gutil_texquad_rt_any(pointer_x,pointer_y,glm_rad(pointer_angle),0,0,256,256,48,48);}
}

/* ---- Parent forms and their isolated field editor ----------------------- */
static bool mps_parent_valid(void) {
	if(view == MPS_VIEW_EDIT_NAME) return mps_username_valid(username_edit_buf);
	if(view == MPS_VIEW_DIRECT_CONNECT) return mps_direct_addr_valid();
	return mps_validate_entry(edit_name_buf, edit_addr_buf, NULL);
}
static bool mps_parent_enabled(int target) {
	return target >= SUB_TARGET_COMMIT && target <=
		(view == MPS_VIEW_EDIT_SERVER ? SUB_TARGET_FIELD_B : SUB_TARGET_FIELD_A)
		&& (target != SUB_TARGET_COMMIT || mps_parent_valid());
}
static struct fep_rect mps_parent_rect(int target) {
	if(target == SUB_TARGET_CANCEL) return mps_rect(104,346,196,44);
	if(target == SUB_TARGET_COMMIT) return mps_rect(320,346,216,44);
	return mps_rect(104,target == SUB_TARGET_FIELD_B ? 236 : 152,432,44);
}
static void mps_open_field(int field, bool preserve_keyboard) {
	if(view == MPS_VIEW_EDIT_SERVER) mps_edit_server_focus(field);
	snprintf(field_snapshot,sizeof(field_snapshot),"%s",osk.edit.buf);
	if(!preserve_keyboard) {
		char* buf = osk.edit.buf; size_t cap = osk.edit.buf_cap;
		int max = osk.edit.beta_max; bool address = osk.address;
		osk_frontend_reset(&osk,buf,cap,max,0,address);
	}
	field_editor_open = true;
	input_text_enable(true);
}
static void mps_close_field(bool cancel) {
	if(cancel) snprintf(osk.edit.buf,osk.edit.buf_cap,"%s",field_snapshot);
	input_text_enable(false);
	field_editor_open = false;
	sub_focus = SUB_TARGET_FIELD_A + (view == MPS_VIEW_EDIT_SERVER ? edit_focus : 0);
	sub_origin = GUTIL_MENU_ORIGIN_DIRECTIONAL;
	/* Re-arm at the layer boundary: held Done/Cancel cannot act on the form. */
	sub_seen = sub_connected = false;
	sub_previous_down = sub_blocked = 0;
	sub_pointer = (struct gutil_menu_focus){0};
}
static unsigned mps_parent_edges(const struct input_player_frame* owner) {
	static const enum input_button roles[] = {IB_GUI_LEFT,IB_GUI_RIGHT,
		IB_GUI_UP,IB_GUI_DOWN,IB_GUI_CLICK,IB_INVENTORY,IB_HOME};
	if(!owner || !owner->connected) { sub_connected = false; return 0; }
	bool fresh = !sub_seen || !sub_connected || sub_category != (int)owner->category;
	unsigned edges = 0, down = 0;
	for(unsigned i=0;i<sizeof(roles)/sizeof(roles[0]);i++) {
		unsigned bit=1u<<i; bool held=owner->down[roles[i]];
		if(held) down |= bit;
		if(fresh && held) sub_blocked |= bit;
		if(!held) sub_blocked &= ~bit;
		if(owner->pressed[roles[i]] && !(sub_blocked & bit) && !(sub_previous_down & bit)) edges |= bit;
	}
	sub_seen=sub_connected=true; sub_category=owner->category; sub_previous_down=down;
	return edges;
}
static void mps_update_parent(struct screen* s, float dt) {
	(void)s; (void)dt;
	pointer_available=input_pointer(&pointer_x,&pointer_y,&pointer_angle);
	struct input_player_frame scratch;
	const struct input_player_frame* owner=osk_frontend_input_frame(&scratch);
	if(field_editor_open) {
		enum osk_event ev=osk_frontend_update_owned(&osk,gfx_width(),gfx_height(),owner,
			true,pointer_available,pointer_x,pointer_y,true,true);
		if(ev==OSK_EV_CANCEL) mps_close_field(true);
		else if(ev==OSK_EV_DONE) mps_close_field(false);
		else if(ev==OSK_EV_NEXT) {
			if(view==MPS_VIEW_EDIT_SERVER) mps_open_field(!edit_focus,true);
			else mps_close_field(false);
		}
		return;
	}
	unsigned edges=mps_parent_edges(owner);
	bool nav=(edges&15)!=0;
	if(nav) {
		bool two=view==MPS_VIEW_EDIT_SERVER;
		if(edges&3) {
			if(sub_focus<=SUB_TARGET_CANCEL && mps_parent_valid())
				sub_focus=sub_focus==SUB_TARGET_CANCEL ? SUB_TARGET_COMMIT : SUB_TARGET_CANCEL;
		} else if(edges&4) {
			sub_focus=sub_focus<=SUB_TARGET_CANCEL ? (two?SUB_TARGET_FIELD_B:SUB_TARGET_FIELD_A)
				: sub_focus==SUB_TARGET_FIELD_B ? SUB_TARGET_FIELD_A : SUB_TARGET_CANCEL;
		} else {
			sub_focus=sub_focus==SUB_TARGET_FIELD_A && two ? SUB_TARGET_FIELD_B
				: sub_focus>=SUB_TARGET_FIELD_A ? SUB_TARGET_CANCEL : SUB_TARGET_FIELD_A;
		}
		sub_origin=GUTIL_MENU_ORIGIN_DIRECTIONAL;
	}
	int hovered=GUTIL_MENU_TARGET_NONE;
	for(int t=SUB_TARGET_COMMIT;pointer_available && t<=
		(view==MPS_VIEW_EDIT_SERVER?SUB_TARGET_FIELD_B:SUB_TARGET_FIELD_A);t++) {
		struct fep_rect r=mps_parent_rect(t);
		if(pointer_x>=r.x && pointer_x<r.x+r.w && pointer_y>=r.y && pointer_y<r.y+r.h) hovered=t;
	}
	if(gutil_menu_pointer_focus(&sub_pointer,pointer_available,pointer_x,pointer_y,nav)
	   && mps_parent_enabled(hovered)) { sub_focus=hovered; sub_origin=GUTIL_MENU_ORIGIN_POINTER; }
	if(view==MPS_VIEW_EDIT_SERVER && sub_focus>=SUB_TARGET_FIELD_A
	   && edit_focus!=sub_focus-SUB_TARGET_FIELD_A)
		mps_edit_server_focus(sub_focus-SUB_TARGET_FIELD_A);
	if(nav) return;
	if(edges&16) {
		struct gutil_menu_decision d=gutil_menu_activate_decide((struct gutil_menu_activate_state){
			.pressed=true, .pointer_valid=pointer_available,
			.selection_origin=sub_origin, .hovered=hovered,
			.hovered_enabled=mps_parent_enabled(hovered), .selected=sub_focus,
			.selected_enabled=mps_parent_enabled(sub_focus), .selected_destructive=false});
		sub_origin=d.selection_origin;
		if(d.action==GUTIL_MENU_FOCUS_ONLY) {
			sub_focus=d.target;
			if(view==MPS_VIEW_EDIT_SERVER && sub_focus>=SUB_TARGET_FIELD_A)
				mps_edit_server_focus(sub_focus-SUB_TARGET_FIELD_A);
		}
		if(d.action==GUTIL_MENU_ACTIVATE) {
			audio_play_ui(SOUND_RANDOM_CLICK,1.0F,1.0F);
			if(d.target>=SUB_TARGET_FIELD_A) mps_open_field(d.target-SUB_TARGET_FIELD_A,false);
			else if(d.target==SUB_TARGET_CANCEL) mps_return_to_main();
			else if(view==MPS_VIEW_EDIT_NAME) mps_try_save_name();
			else if(view==MPS_VIEW_DIRECT_CONNECT) mps_try_direct_connect();
			else mps_try_save_server();
		}
		return; /* A shared Activate/Close edge is consumed exactly once. */
	}
	if(edges&96) mps_return_to_main();
}
static void mps_parent_control(int target,const char* label) {
	struct fep_rect r=mps_parent_rect(target);
	bool enabled=mps_parent_enabled(target), hot=enabled && sub_focus==target;
	bool field=target>=SUB_TARGET_FIELD_A;
	bool quiet=target==SUB_TARGET_CANCEL && !hot;
	if(field) { fep_fill(r,37,38,35,255); fep_outline(r,mps_px(2),85,85,85,255); }
	else if(quiet) {
		fep_paper(r); fep_outline(r,mps_px(1),85,85,85,255);
		fep_fill(fep_rect(r.x,r.y+r.h-mps_px(2),r.w,mps_px(2)),85,85,85,255);
	} else fep_widget(r,hot);
	if(hot) fep_outline(r,mps_px(2),255,255,160,255);
	if(!enabled) fep_fill(r,0,0,0,110);
	/* No glyph inside the button (controller UI redesign 10-31): its role
	 * is named in the centred hint row below. */
	int font=mps_px(field?19:18); char text[256];
	(void)fep_text_fit(text,sizeof(text),label,font,r.w-mps_px(32));
	int group=gutil_font_width(text,font);
	int x=target>=SUB_TARGET_FIELD_A ? r.x+mps_px(16) : r.x+(r.w-group)/2;
	gutil_text_rgb(x,r.y+(r.h-font)/2,text,font,!field && !quiet,
		quiet?53:hot&&!field?255:enabled?244:160,
		quiet?53:hot&&!field?255:enabled?244:160,
		quiet?53:hot&&!field?160:enabled?237:160);
}
static void mps_render_field(int width,int height) {
	if(!fep_backdrop(width,height,90)) gutil_bg();
	struct fep_rect panel=mps_rect(20,16,600,448); fep_paper(panel); fep_bevel(panel);
	const char* title=view==MPS_VIEW_EDIT_NAME?"Player name":osk.address?"Server address":"Server name";
	mps_text(32,28,27,440,title,true);
	char count[32]; size_t cap=osk.edit.beta_max>0?(size_t)osk.edit.beta_max:osk.edit.buf_cap-1;
	snprintf(count,sizeof(count),"%u / %u",(unsigned)strlen(osk.edit.buf),(unsigned)cap);
	int cf=mps_px(14); struct fep_rect right=mps_rect(608,37,0,0);
	gutil_text_rgb(right.x-gutil_font_width(count,cf),right.y,count,cf,false,53,53,53);
	struct fep_rect field=mps_rect(32,76,576,52);
	fep_fill(field,25,26,23,255); fep_outline(field,mps_px(1),85,85,85,255);
	fep_fill(fep_rect(field.x,field.y+field.h-mps_px(2),field.w,mps_px(2)),184,188,178,255);
	int pad=mps_px(16),font=mps_px(20),cw=mps_px(2);
	size_t start=osk_frontend_reveal(&osk,font,field.w-2*pad-cw);
	int x=field.x+pad,y=field.y+(field.h-font)/2,cx=x;
	gfx_scissor(true,(uint32_t)x,(uint32_t)field.y,(uint32_t)(field.w-2*pad),(uint32_t)field.h);
	gutil_text_rgb(x,y,osk.edit.buf+start,font,false,244,244,237);
	for(size_t i=start;i<osk.caret;i++){char ch[2]={osk.edit.buf[i],0};cx+=gutil_font_width(ch,font);}
	fep_fill(fep_rect(cx,y,cw,font),255,255,160,255); gfx_scissor(false,0,0,0,0);
	if(osk.address) mps_text(32,136,14,576,"Host name or IP - port optional",true);
	else if(view==MPS_VIEW_EDIT_NAME) mps_text(32,136,14,576,"Letters, numbers and _ only. Choose your own name.",true);
	osk_frontend_render(&osk,width,height,true);
	struct input_player_frame prompts; input_prompt_frame(0,&prompts);
	osk_frontend_hint_row(&osk,&prompts,width,height);
}
static void mps_render_parent(int width,int height) {
	if(field_editor_open) mps_render_field(width,height);
	else {
		mps_render_main(width,height);
		fep_fill(fep_rect(0,0,width,height),0,0,0,160);
		struct fep_rect panel=mps_rect(70,62,500,356); fep_paper(panel); fep_bevel(panel);
		const char* title=view==MPS_VIEW_EDIT_NAME?"Player name":view==MPS_VIEW_DIRECT_CONNECT?"Direct Connect":edit_is_new?"Add Server":"Edit Server";
		int font=mps_px(24); struct fep_rect head=mps_rect(102,84,436,24);
		gutil_text_rgb(head.x+(head.w-gutil_font_width(title,font))/2,head.y,title,font,false,53,53,53);
		bool server=view==MPS_VIEW_EDIT_SERVER;
		mps_text(104,130,14,432,view==MPS_VIEW_EDIT_NAME?"Player name":server?"Server name":"Server address",true);
		const char* first=server?edit_name_buf:view==MPS_VIEW_EDIT_NAME?username_edit_buf:direct_addr_buf;
		mps_parent_control(SUB_TARGET_FIELD_A,*first?first:server?"New Server":view==MPS_VIEW_EDIT_NAME?"Choose a player name":"Address and port");
		if(server) {
			mps_text(104,214,14,432,"Server address",true);
			mps_parent_control(SUB_TARGET_FIELD_B,*edit_addr_buf?edit_addr_buf:"Address and port");
		}
		const char* hint=view==MPS_VIEW_EDIT_NAME?(join_pending?"Choose a player name to join":"Letters, numbers and _ only") : "Host name or IP - port optional";
		bool invalid=false;
		const char* addr=server?edit_addr_buf:direct_addr_buf;
		if(view!=MPS_VIEW_EDIT_NAME && *addr) {
			char trimmed[CONFIG_MP_SERVER_ADDRESS_CAP],host[MP_ADDRESS_HOST_CAPACITY];
			uint16_t port;
			screen_cworld_java_trim(addr,trimmed,sizeof(trimmed));
			if(!mp_address_parse(trimmed,host,sizeof(host),&port) || !host[0]) {
				hint="Enter a valid host name or IP and optional port"; invalid=true;
			}
		}
		if(view==MPS_VIEW_EDIT_NAME && *first && !mps_username_valid(first)) {
			hint="Use letters, numbers and _; choose your own name"; invalid=true;
		}
		char hint_text[128]; int hint_font=mps_px(15);
		(void)fep_text_fit(hint_text,sizeof(hint_text),hint,hint_font,mps_px(432));
		struct fep_rect hint_at=mps_rect(104,server?286:202,432,15);
		gutil_text_rgb(hint_at.x,hint_at.y,hint_text,hint_font,false,
			invalid?121:53,invalid?44:53,invalid?37:53);
		mps_parent_control(SUB_TARGET_CANCEL,"Back");
		mps_parent_control(SUB_TARGET_COMMIT,view==MPS_VIEW_DIRECT_CONNECT?"Connect":"Save");
		(void)mps_footer_row(true);
	}
	if(pointer_available){gfx_bind_texture(&texture_pointer);gutil_texquad_rt_any(pointer_x,pointer_y,glm_rad(pointer_angle),0,0,256,256,48,48);}
}
static void mps_update_edit_name(struct screen* s,float dt){mps_update_parent(s,dt);}
static void mps_update_edit_server(struct screen* s,float dt){mps_update_parent(s,dt);}
static void mps_update_direct_connect(struct screen* s,float dt){mps_update_parent(s,dt);}
static void mps_render_edit_name(int w,int h){mps_render_parent(w,h);}
static void mps_render_edit_server(int w,int h){mps_render_parent(w,h);}
static void mps_render_direct_connect(int w,int h){mps_render_parent(w,h);}

/* ---- Dispatch ------------------------------------------------------------- */

static void screen_mps_update(struct screen* s, float dt) {
	if(dt > 0.0F && view == MPS_VIEW_MAIN) menu_motion_time = fmodf(menu_motion_time + dt, 128.0F);
#ifdef PLATFORM_WII
	mps_ping_poll_bringup();
#endif
	mps_ping_tick();
	switch(view) {
		case MPS_VIEW_MAIN: mps_update_main(s, dt); break;
		case MPS_VIEW_EDIT_NAME: mps_update_edit_name(s, dt); break;
		case MPS_VIEW_EDIT_SERVER: mps_update_edit_server(s, dt); break;
		case MPS_VIEW_DIRECT_CONNECT: mps_update_direct_connect(s, dt); break;
	}
}

static void screen_mps_render2D(struct screen* s, int width, int height) {
	(void)s;
	switch(view) {
		case MPS_VIEW_MAIN: mps_render_main(width, height); break;
		case MPS_VIEW_EDIT_NAME: mps_render_edit_name(width, height); break;
		case MPS_VIEW_EDIT_SERVER:
			mps_render_edit_server(width, height);
			break;
		case MPS_VIEW_DIRECT_CONNECT:
			mps_render_direct_connect(width, height);
			break;
	}
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void screen_mps_test_reset_servers(void) { server_count = 0; }

size_t screen_mps_test_server_count(void) { return server_count; }

bool screen_mps_test_server(size_t idx, char* name, size_t name_cap,
							char* address, size_t address_cap) {
	if(idx >= server_count)
		return false;
	snprintf(name, name_cap, "%s", servers[idx].name);
	snprintf(address, address_cap, "%s", servers[idx].address);
	return true;
}

bool screen_mps_test_add_server(const char* name, const char* address) {
	return mps_array_add(name, address);
}

bool screen_mps_test_edit_server(size_t idx, const char* name,
								 const char* address) {
	return mps_array_edit(idx, name, address);
}

bool screen_mps_test_delete_server(size_t idx) {
	return mps_array_delete(idx);
}

bool screen_mps_test_username_valid(const char* text) {
	return mps_username_valid(text);
}

/* Join gate probe: install `name` as the current player name and press
 * Join on a throwaway target. Reports whether the name editor opened with
 * the join parked. Only meaningful for a NOT-chosen name -- a chosen one
 * would hand off to the real connect screen. */
bool screen_mps_test_join_gate(const char* name, bool* editor_opened,
							   bool* parked) {
	snprintf(username_buf, sizeof(username_buf), "%s", name ? name : "");
	view = MPS_VIEW_MAIN;
	join_pending = false;
	if(mp_username_chosen(username_buf))
		return false;
	struct config_mp_server target;
	memset(&target, 0, sizeof(target));
	snprintf(target.address, sizeof(target.address), "%s", "127.0.0.1:1");
	mps_activate(&target);
	if(editor_opened)
		*editor_opened = view == MPS_VIEW_EDIT_NAME;
	if(parked)
		*parked = join_pending;
	return true;
}

/* Cancel out of the gate's editor: the parked join must be gone. */
bool screen_mps_test_join_gate_cancel(void) {
	mps_return_to_main();
	return !join_pending && view == MPS_VIEW_MAIN;
}

bool screen_mps_test_parse_ping_response(const uint8_t* data, size_t len,
										 char* motd_out, size_t motd_cap,
										 int32_t* online_out,
										 int32_t* max_out) {
	struct mps_ping_status status;
	bool ok = mps_ping_parse_response(data, len, &status);
	if(ok) {
		if(motd_out)
			snprintf(motd_out, motd_cap, "%s", status.motd);
		if(online_out)
			*online_out = status.online;
		if(max_out)
			*max_out = status.max;
	}
	return ok;
}

int screen_mps_test_ping_bars(int32_t latency_ms) {
	return mps_ping_bars_for_latency(latency_ms);
}

void screen_mps_test_set_ping_cursor(size_t cursor) { ping_cursor = cursor; }

bool screen_mps_test_refresh_enabled(void) {
	return mps_button_enabled(MPS_BUTTON_REFRESH);
}

int screen_mps_test_ping_toctou_decide(bool done_now,
									   bool deadline_now_reached) {
	return (int)mps_ping_toctou_decide(done_now, deadline_now_reached);
}

/* Read-only observation points for tests/test_frontend_no_pointer.c: the
 * viewport offset a page role moves, the row it must not lose, which subview
 * an activation opened, and the join parked behind the name gate -- the one
 * thing that distinguishes Quick Connect (servers[0]) from a Join on the
 * selected row. Nothing here changes state. */
int screen_mps_test_scroll(void) {
 return (int)mps_page * mps_px(4 * 48);
}
size_t screen_mps_test_page(void);
size_t screen_mps_test_page(void) { return mps_page; }
bool screen_mps_test_options_open(void);
bool screen_mps_test_options_open(void) { return mps_options_open; }
int screen_mps_test_list_bottom(void);
int screen_mps_test_list_bottom(void) { return mps_px(302); }
bool screen_mps_test_target_rect(int target, struct gutil_button_bounds* out);
bool screen_mps_test_target_rect(int target, struct gutil_button_bounds* out) {
 struct fep_rect r=mps_target_rect(target);
 if(out)*out=(struct gutil_button_bounds){r.x,r.y,r.w,r.h};
 return r.w>0&&r.h>0;
}

size_t screen_mps_test_selection(void) { return gui_selection; }

/* The row widget_slotlist.c's render draws the selection chrome on (A15) --
 * the seen state, next to gui_selection's intended one. */
size_t screen_mps_test_list_selection(void) {
	return slotlist_selected(&server_slots);
}

int screen_mps_test_view(void) { return (int)view; }

/* Where directional focus rests: a MPS_BUTTON_* index, or one of the negative
 * MPS_FOCUS_* ring positions. Read-only. */
int screen_mps_test_focus(void) { return mps_focus; }

/* Whether that resting place is something the player can actually activate --
 * the property review finding M-2 is about. */
bool screen_mps_test_focus_enabled(void) {
	/* The SAME predicate the activation decision is fed, so this hook cannot
	 * describe a different screen than the one that ships. */
	return mps_target_enabled(mps_focus_target());
}

bool screen_mps_test_pending_join(char* address, size_t cap) {
	if(address && cap)
		snprintf(address, cap, "%s", join_pending ? pending_join.address : "");
	return join_pending;
}

/* WHICH row Edit and Delete armed (push-0921 A15). Both read gui_selection at
 * activation time and latch it -- edit_index/edit_is_new for the editor,
 * delete_index for the confirmation -- so these two read-only hooks are what
 * turn "the strip is reachable" into "the strip acts on the row the player
 * selected". The name/address are the editor's own prefilled buffers, i.e.
 * the server the player is about to edit. */
bool screen_mps_test_edit_target(size_t* index, bool* is_new, char* name,
								 size_t name_cap, char* address,
								 size_t address_cap) {
	if(index)
		*index = edit_index;
	if(is_new)
		*is_new = edit_is_new;
	if(name && name_cap)
		snprintf(name, name_cap, "%s", edit_name_buf);
	if(address && address_cap)
		snprintf(address, address_cap, "%s", edit_addr_buf);
	return view == MPS_VIEW_EDIT_SERVER;
}

size_t screen_mps_test_delete_index(void) { return delete_index; }

/* Is the sticky save-failure line up? (A23, review A20-3.) It is the flag the
 * render itself branches on, so a test can state the precondition of the
 * overlap rule -- "the failure line is visible AND the hint is not" -- rather
 * than assuming it. Read-only. */
bool screen_mps_test_save_failed(void) { return mps_save_failed; }

/* THE NAVIGATION HINT as the render draws it (A19) -- same pure decision,
 * same rect function, plus the two edges it must stay between. Read-only. */
bool screen_mps_test_hint(char* text, size_t cap, int* y, int* h,
						  int* list_bottom, int* strip_top) {
	int width = gfx_width(), height = gfx_height();
	const char* hint = mps_nav_hint();
	struct gutil_button_bounds r
		= gutil_menu_nav_hint_rect(width, mps_strip_top(width, height), hint);
	if(text && cap)
		snprintf(text, cap, "%s", hint ? hint : "");
	if(y)
		*y = r.y;
	if(h)
		*h = r.height;
	if(list_bottom)
		*list_bottom = mps_px(302);
	if(strip_top)
		*strip_top = mps_strip_top(width, height);
	return hint != NULL && r.width > 0;
}

/* D3: parent field focus and the bound draft move together on a focus-only
 * press; activating the focused field opens the editor. These observers read
 * that production state without simulating input or opening a field. */
/* Append inside the existing CAVEWII_CLIENT_INTERFACE_TEST block. Read-only.
 * Targets: Commit=1, Back=2, first field=3, address field in Add/Edit=4. */
bool screen_mps_test_editor_open(void) { return field_editor_open; }
int screen_mps_test_parent_focus(void) { return sub_focus; }
bool screen_mps_test_parent_rect(int target, struct gutil_button_bounds* out) {
	if(!out || view==MPS_VIEW_MAIN || target<SUB_TARGET_COMMIT || target>
		(view==MPS_VIEW_EDIT_SERVER?SUB_TARGET_FIELD_B:SUB_TARGET_FIELD_A)) return false;
	struct fep_rect r=mps_parent_rect(target);
	*out=(struct gutil_button_bounds){.x=r.x,.y=r.y,.width=r.w,.height=r.h};
	return true;
}
int screen_mps_test_editor_target(void) { return osk.target; }
size_t screen_mps_test_editor_caret(void) { return osk.caret; }

int screen_mps_test_edit_field(void) { return edit_focus; }

bool screen_mps_test_osk_writes_address(void) {
	return osk.edit.buf == edit_addr_buf;
}
#endif

struct screen screen_multiplayer_setup_impl = {
	.reset = mps_reset,
	.update = screen_mps_update,
	.render2D = screen_mps_render2D,
	.render3D = NULL,
	.render_world = false,
};

#else /* !CAVEWII_MP_CLIENT */

/* No MP client on this platform; the symbol exists so main.c's registration
 * line can take its address unconditionally (same shape as
 * screen_multiplayer_connect_impl's own #else half). */
struct screen screen_multiplayer_setup_impl = {0};

#endif
