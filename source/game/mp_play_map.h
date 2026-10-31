/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

#ifndef GAME_MP_PLAY_MAP_H
#define GAME_MP_PLAY_MAP_H

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../block/blocks_data.h" /* enum side */
#include "../block/aabb.h" /* collision box of the intended landing support */
#include "../item/inventory.h"	  /* the client's own window-0 slot layout */
#include "../network/server_remote.h"
#include "../platform/input.h" /* enum input_button */
#include "mp_play_core.h"

/* NOT chat_login_script.h, on purpose: that header's own includes
 * (input_owner.h / input_inventory.h) drag in ptime_t, which exists only under
 * PLATFORM_PC / PLATFORM_WII -- pulling the whole console input stack into a
 * header whose entire claim is that it is PURE and testable off the engine.
 * The one thing borrowed from it is the request cap, restated below and PINNED
 * to the original by a compile-time check in mp_play_script.c. The platform
 * word is NOT restated: it is a PARAMETER of the marker builder, so
 * CHAT_LOGIN_PLATFORM stays the only definition of it in the tree. */

/* Must equal CHAT_LOGIN_PLAY_ACTIONS (chat_login_script.h). */
#define MPP_MAP_MAX_ACTIONS 12

/* Driver-only dig admission. The one-block drop after removing the block at
 * feet-1 can land at the intended feet-1 height only when the block below it
 * has a full, ordinary collision cube. This is deliberately conservative:
 * the script skips odd/unstable footing; it changes no world collision rule. */
static inline bool mpp_map_support_full_cube(bool known, uint8_t type,
											 size_t box_count, const struct AABB* box) {
	if(!known || !box || box_count != 1u)
		return false;
	switch(type) {
		case BLOCK_AIR:
		case BLOCK_WATER_FLOW: case BLOCK_WATER_STILL:
		case BLOCK_LAVA_FLOW: case BLOCK_LAVA_STILL:
		case BLOCK_FIRE: case BLOCK_CACTUS:
		case BLOCK_SAND: case BLOCK_GRAVEL:
			return false;
		default: break;
	}
	return isfinite(box->x1) && isfinite(box->y1) && isfinite(box->z1)
		&& isfinite(box->x2) && isfinite(box->y2) && isfinite(box->z2)
		&& box->x1 == 0.0f && box->y1 == 0.0f && box->z1 == 0.0f
		&& box->x2 == 1.0f && box->y2 == 1.0f && box->z2 == 1.0f;
}

/*
	The PURE half of mp_play_script.c: every translation between the core's
	own vocabulary (source/game/mp_play_core.h) and the production world it is
	driven against. Kept out of the .c -- and unit-tested on its own in
	tests/test_mp_play_map.c -- because each of these is a table or an index
	arithmetic that is either right or wrong for ALL inputs, and proving that
	needs no engine, no screen and no socket.

	Nothing here reads or writes any global. Every function is a function of
	its arguments alone.

	Eight groups:
	  1. ACTIONS  -- MPP_ACT_* bit -> enum input_button, and a held mask ->
	                 the action LIST chat_login_play_request takes.
	  2. SLOTS    -- the three slot numberings this profile has to keep
	                 straight (core inv[] index, the client's own player
	                 inventory index, the protocol window-0 slot number the
	                 fixture's receipts print).
	  3. RINGS    -- struct server_remote_status's observation rings ->
	                 struct mpp_obs's echo / txn arrays, newest first, by
	                 SERIAL rather than by ring index so the copy stays
	                 correct if either capacity ever changes.
	  4. MARKERS  -- struct mpp_marker -> the telemetry line the collector
	                 parses.
	  5. BODY     -- the local player's velocity/ground reading -> the core's
	                 `at_rest`, the one observation the whole build leg turns
	                 on.
	  6. YAW      -- the core's Beta yaw convention -> the engine convention.
	  7. WINDOW   -- the next container action id -> the last sent id.
	  8. LOAD     -- the approved Load screen's semantic focus -> the real
	                 directional input that advances toward Join.
	  9. MP SETUP -- preserve the chosen saved-server row while advancing to
	                 its Join button.
*/

/* ---- 1. actions ---------------------------------------------------------- */

/* The core's whole vocabulary, in bit order. `mpp_map_action` is the ONE place
 * an abstract action becomes a production input role; chat_login_play_request
 * then resolves that role through the LIVE binding table, so which physical
 * code carries it stays the staged config's decision.
 *
 * MPP_ACT_OPEN_INVENTORY is IB_OPEN_INVENTORY, not IB_INVENTORY: b1-chat-
 * opener-0919/07 C split the gameplay "open the inventory" role out of
 * IB_INVENTORY precisely because that one role used to mean both opening a
 * window and Close/Back inside one. MPP_ACT_CLOSE keeps IB_INVENTORY, which is
 * the role that kept every Close call site.
 *
 * MPP_ACT_FORWARD is IB_FORWARD, the SAME role the walker forces
 * (autopilot_force_button). It is safe for the core to press it as a single
 * 50 ms centring tap because that only ever happens in BUILD_REST, where the
 * impure half is asserting the AIM lock -- autopilot mode 1, which "forces
 * nothing" (autopilot.c:271-273) -- so input_held resolves IB_FORWARD from
 * the driver's own pad and the walker is not fighting the tap. */
#define MPP_MAP_ACTION_COUNT 13u

/* The Load screen owns its concrete focus enum. The impure half translates
 * that enum into these semantic locations, then this pure decision supplies
 * the real input role. A selected row is therefore one RIGHT away from Play,
 * regardless of whether it is row 0 or row 649. */
enum mpp_load_focus {
	MPP_LOAD_FOCUS_OTHER = 0,
	MPP_LOAD_FOCUS_LIST,
	MPP_LOAD_FOCUS_TAB_LOAD,
	MPP_LOAD_FOCUS_TAB_CREATE,
	MPP_LOAD_FOCUS_TAB_JOIN,
	MPP_LOAD_FOCUS_PREV,
	MPP_LOAD_FOCUS_NEXT,
	MPP_LOAD_FOCUS_PLAY,
	MPP_LOAD_FOCUS_RETRY,
	MPP_LOAD_FOCUS_OPTIONS,
};

static inline enum input_button mpp_map_load_route_button(
	enum mpp_load_focus focus) {
	switch(focus) {
		case MPP_LOAD_FOCUS_LIST: return IB_GUI_RIGHT;
		case MPP_LOAD_FOCUS_TAB_LOAD:
		case MPP_LOAD_FOCUS_TAB_CREATE: return IB_GUI_RIGHT;
		case MPP_LOAD_FOCUS_PREV:
		case MPP_LOAD_FOCUS_NEXT:
		case MPP_LOAD_FOCUS_PLAY:
		case MPP_LOAD_FOCUS_RETRY:
		case MPP_LOAD_FOCUS_OPTIONS: return IB_GUI_DOWN;
		default: return IB_GUI_UP;
	}
}

/* The Join screen opens on its tab with the intended saved row already
 * selected. DOWN enters that row. From the row, RIGHT reaches Join without
 * changing the selection; keep RIGHT held after focus reaches Join so the
 * core's 120 ms abstract press cannot create a fresh DOWN edge on the action
 * strip before its neutral gap. */
static inline enum input_button mpp_map_mps_route_step(bool* advanced) {
	bool keep_right = advanced && *advanced;
	if(advanced)
		*advanced = true;
	return keep_right ? IB_GUI_RIGHT : IB_GUI_DOWN;
}

static inline void mpp_map_mps_route_reset(bool* advanced) {
	if(advanced)
		*advanced = false;
}

static inline bool mpp_map_action(unsigned bit, enum input_button* out) {
	enum input_button b;
	switch(bit) {
		case MPP_ACT_ATTACK: b = IB_ACTION1; break;
		case MPP_ACT_USE: b = IB_ACTION2; break;
		case MPP_ACT_OPEN_INVENTORY: b = IB_OPEN_INVENTORY; break;
		case MPP_ACT_CLOSE: b = IB_INVENTORY; break;
		case MPP_ACT_PAUSE: b = IB_HOME; break;
		case MPP_ACT_GUI_UP: b = IB_GUI_UP; break;
		case MPP_ACT_GUI_DOWN: b = IB_GUI_DOWN; break;
		case MPP_ACT_GUI_LEFT: b = IB_GUI_LEFT; break;
		case MPP_ACT_GUI_RIGHT: b = IB_GUI_RIGHT; break;
		case MPP_ACT_GUI_CLICK: b = IB_GUI_CLICK; break;
		case MPP_ACT_HOTBAR_NEXT: b = IB_SCROLL_RIGHT; break;
		case MPP_ACT_FORWARD: b = IB_FORWARD; break;
		case MPP_ACT_JUMP: b = IB_JUMP; break;
		default: return false;
	}
	if(out)
		*out = b;
	return true;
}

/* A held MPP_ACT_* mask -> the request list, in bit order (lowest bit first,
 * so the list is a deterministic function of the mask and a trace of two runs
 * can be compared byte for byte).
 *
 * Returns the count written, or -1 when the mask carries a bit that is not in
 * the vocabulary, or when the list would not fit `cap`. REFUSED, never
 * truncated: a dropped action is a press the core believes it made, and
 * chat_login_play_request refuses an over-long list for exactly that reason. */
static inline int mpp_map_actions(unsigned mask, enum input_button* out,
								  unsigned cap) {
	unsigned n = 0;
	if(!out)
		return -1;
	for(unsigned k = 0; k < MPP_MAP_ACTION_COUNT; k++) {
		enum input_button b;
		unsigned bit = 1u << k;
		if(!(mask & bit))
			continue;
		if(!mpp_map_action(bit, &b))
			return -1;
		if(n >= cap)
			return -1;
		out[n++] = b;
	}
	/* Any bit above the vocabulary is a caller error, not something to ignore. */
	if(mask >> MPP_MAP_ACTION_COUNT)
		return -1;
	return (int)n;
}

/* ---- 2. slots and faces -------------------------------------------------- */

/*
	THREE numberings, and this profile has to keep them straight in both
	directions:

	  core index    mp_play_core.h's inv[]: 0..8 the hotbar, 9..35 the main
	                grid (MPP_INV_SLOTS = 36 -- the player's own stacks and
	                nothing else).
	  client index  a subscript into the client's own player-inventory array,
	                struct inventory::items. That array IS the Beta window-0
	                layout (source/item/inventory.h): 0 craft output, 1..4
	                crafting, 5..8 armor, 9..35 main, 36..44 hotbar.
	  protocol slot the wire/receipt number mc173-diag prints. Beta's window 0
	                uses exactly the same layout.

	Client index and protocol slot are therefore the SAME integer today. They
	are still two functions, because they are two different CONCEPTS (an array
	subscript vs. a wire value): a client that ever re-ordered its own array
	would have one of them change and the other not, and the round-trip test
	would say so instead of this header silently lying.

	Every conversion returns -1 for anything outside its domain -- in
	particular for the armor / crafting / output slots, which are NOT among the
	player's 36 and have no core index at all.
*/

static inline int mpp_map_core_to_client(int core) {
	if(core < 0 || core >= MPP_INV_SLOTS)
		return -1;
	return core < 9 ? INVENTORY_SLOT_HOTBAR + core : INVENTORY_SLOT_MAIN + (core - 9);
}

static inline int mpp_map_client_to_core(int client) {
	if(client >= INVENTORY_SLOT_HOTBAR
	   && client < INVENTORY_SLOT_HOTBAR + INVENTORY_SIZE_HOTBAR)
		return client - INVENTORY_SLOT_HOTBAR;
	if(client >= INVENTORY_SLOT_MAIN
	   && client < INVENTORY_SLOT_MAIN + INVENTORY_SIZE_MAIN)
		return 9 + (client - INVENTORY_SLOT_MAIN);
	return -1; /* output / crafting / armor, or out of range */
}

/* The core already has mpp_proto_slot() for one direction; these two are the
 * validated pair, and the test proves all three agree for all 36. */
static inline int mpp_map_core_to_proto(int core) {
	if(core < 0 || core >= MPP_INV_SLOTS)
		return -1;
	return mpp_proto_slot(core);
}

static inline int mpp_map_proto_to_core(int proto) {
	return mpp_map_client_to_core(proto);
}

/* enum side (engine-internal, blocks_side_offset's axis mapping) -> the Beta
 * PROTOCOL face number the core's MPP_FACE_TOP is written in.
 *
 * Matched by AXIS DELTA, never by casting the ordinal: Beta's ItemBlock.
 * onItemUse numbers 0 -Y, 1 +Y, 2 -Z, 3 +Z, 4 -X, 5 +X [740c583
 * ItemBlock.onItemUse], while CaveWii's own enum is TOP +Y, BOTTOM -Y,
 * LEFT -X, RIGHT +X, BACK +Z, FRONT -Z. This is the same table
 * remote_side_to_face() encodes in source/network/server_remote_protocol.c:
 * 156-172 -- that one is a file static in the MP pool, which this engine-pool
 * header may not call, so the pair is pinned by the round-trip test rather
 * than by a shared symbol. Returns -1 for an unhandled side rather than
 * guessing a face: the caller then reports "no usable face", which costs the
 * build leg, where a wrong guess would count a place that never happened. */
static inline int mpp_map_face_of_side(enum side side) {
	switch(side) {
		case SIDE_BOTTOM: return 0; /* -Y */
		case SIDE_TOP: return 1;	/* +Y */
		case SIDE_FRONT: return 2;	/* -Z */
		case SIDE_BACK: return 3;	/* +Z */
		case SIDE_LEFT: return 4;	/* -X */
		case SIDE_RIGHT: return 5;	/* +X */
		default: return -1;
	}
}

/* ---- 3. rings ------------------------------------------------------------ */

/* server_remote.h's documented lookup rule: the record carrying serial `s`
 * lives at ring index `s % capacity`, and a hit is confirmed by `.serial == s`
 * (anything older than the newest `capacity` records has been overwritten).
 * `serial == 0` never names a record -- serials are assigned 1, 2, 3 ...
 *
 * Both copies below walk the NEWEST serials downwards rather than copying the
 * ring index-for-index. That is not decoration: it keeps the copy correct if
 * either capacity ever changes (today both pairs are equal), and it makes the
 * "which records did the driver lose" question answerable -- a serial that
 * does not fit the destination is an OLDER one, never a newer one. The core
 * only ever asks "is there a record with serial > X for this cell / action",
 * so destination ORDER does not matter to it; recency does. */

static inline void mpp_map_copy_echoes(const struct server_remote_play_obs* st,
									   struct mpp_obs* o) {
	size_t cap = sizeof(o->echoes) / sizeof(o->echoes[0]);
	memset(o->echoes, 0, sizeof(o->echoes));
	/* push-0921/B19: the watched cell's slot travels with the ring, and is
	 * cleared on the same paths -- a NULL source ("no remote provider") must
	 * not leave last frame's watched echo standing any more than it may leave
	 * last frame's ring standing. serial 0 = none, which is what the core
	 * reads as "not observed". */
	memset(&o->watch_echo, 0, sizeof(o->watch_echo));
	memset(o->watch_hist, 0, sizeof(o->watch_hist));
	o->watch_hist_n = o->watch_bodies = o->watch_clears = 0u;
	o->echo_serial = st ? st->block_echo_serial : 0u;
	if(!st)
		return;
	/* hw1030g: the watched cell's history and the seam's own counters. */
	for(size_t k = 0; k < SERVER_REMOTE_WATCH_HISTORY
		&& k < sizeof(o->watch_hist) / sizeof(o->watch_hist[0]);
		k++) {
		const struct server_remote_block_echo* h = &st->watch_history[k];
		o->watch_hist[k].x = h->x;
		o->watch_hist[k].y = h->y;
		o->watch_hist[k].z = h->z;
		o->watch_hist[k].type = h->type;
		o->watch_hist[k].metadata = h->metadata;
		o->watch_hist[k].serial = h->serial;
	}
	o->watch_hist_n = st->watch_history_n;
	o->watch_bodies = st->watch_bodies;
	o->watch_clears = st->watch_clears;
	o->watch_echo.x = st->watch_echo.x;
	o->watch_echo.y = st->watch_echo.y;
	o->watch_echo.z = st->watch_echo.z;
	o->watch_echo.type = st->watch_echo.type;
	o->watch_echo.metadata = st->watch_echo.metadata;
	o->watch_echo.serial = st->watch_echo.serial;
	for(size_t k = 0; k < cap; k++) {
		uint32_t want = st->block_echo_serial - (uint32_t)k;
		const struct server_remote_block_echo* e;
		if(st->block_echo_serial < (uint32_t)k + 1u)
			break; /* walked past serial 1: nothing older exists */
		e = &st->block_echoes[want % SERVER_REMOTE_BLOCK_ECHO_RING];
		if(e->serial != want)
			continue; /* overwritten, or never written */
		o->echoes[k].x = e->x;
		o->echoes[k].y = e->y;
		o->echoes[k].z = e->z;
		o->echoes[k].type = e->type;
		/* remote_apply_block_change masks metadata to 4 bits before it reaches
		 * the client world (server_remote.h), so a reader comparing against a
		 * RENDERED block compares the low nibble. The raw byte is carried here
		 * unchanged -- the core only ever compares `type` -- and the masking is
		 * left to whoever compares against the world, so nothing is silently
		 * dropped on the way in. */
		o->echoes[k].metadata = e->metadata;
		o->echoes[k].serial = e->serial;
	}
}

static inline void mpp_map_copy_txns(const struct server_remote_play_obs* st,
									 struct mpp_obs* o) {
	size_t cap = sizeof(o->txns) / sizeof(o->txns[0]);
	memset(o->txns, 0, sizeof(o->txns));
	o->txn_serial = st ? st->txn_serial : 0u;
	if(!st)
		return;
	for(size_t k = 0; k < cap; k++) {
		uint32_t want = st->txn_serial - (uint32_t)k;
		const struct server_remote_txn_result* t;
		if(st->txn_serial < (uint32_t)k + 1u)
			break;
		t = &st->txn_results[want % SERVER_REMOTE_TXN_RING];
		if(t->serial != want)
			continue;
		o->txns[k].window = t->window;
		o->txns[k].action_id = t->action_id;
		o->txns[k].accepted = t->accepted;
		o->txns[k].serial = t->serial;
	}
}

/* THE EMPTY-SLOT CONVENTION, in one place. The set-slot ring writes `item ==
 * -1` for "the stack is absent" (the decoded packet's own `present == false`,
 * which the wire spells id 0xFFFF), with count/damage 0 there. struct mpp_item
 * spells the same thing `item == 0`, because the core's mpp_item_eq treats
 * anything <= 0 as empty and its snapshots omit those slots. Converting in
 * both directions through this one function is what keeps a -1 from ever
 * reaching the core as a real item id. */
static inline struct mpp_item mpp_map_item_from_set_slot(
	const struct server_remote_set_slot* ss) {
	struct mpp_item it;
	memset(&it, 0, sizeof(it));
	if(!ss || ss->item < 0)
		return it; /* absent -> the core's own empty */
	it.item = ss->item;
	it.count = ss->count;
	it.damage = ss->damage;
	return it;
}

/* ---- 4. markers ---------------------------------------------------------- */

/* RFC 8259 escaping for the two strings this module did not author itself (the
 * run id and the run nonce). Same rules as chat_login_script.c's
 * cl_json_escape; duplicated rather than exported because that one is a file
 * static in an APP-shaped driver and this header must stay pure. */
static inline void mpp_map_json_escape(char* dst, size_t cap, const char* src) {
	size_t w = 0;
	if(!dst || !cap)
		return;
	for(size_t r = 0; src && src[r] && w + 1 < cap; r++) {
		unsigned char ch = (unsigned char)src[r];
		const char* two = NULL;
		switch(ch) {
			case '"': two = "\\\""; break;
			case '\\': two = "\\\\"; break;
			case '\n': two = "\\n"; break;
			case '\r': two = "\\r"; break;
			case '\t': two = "\\t"; break;
			case '\b': two = "\\b"; break;
			case '\f': two = "\\f"; break;
			default: break;
		}
		if(two) {
			if(w + 2 >= cap)
				break;
			dst[w++] = two[0];
			dst[w++] = two[1];
		} else if(ch < 0x20 || ch == 0x7F) {
			if(w + 6 >= cap)
				break;
			w += (size_t)snprintf(dst + w, cap - w, "\\u%04X", ch);
		} else
			dst[w++] = (char)ch;
	}
	dst[w] = 0;
}

/* The marker line the collector parses (design section 7: event name
 * `mp_play`, BARE marker keys):
 *
 *   {"event":"mp_play","seq":N,"marker":"<key>",<body>}
 *
 * and, for the `session` marker alone, the two members the core cannot know
 * because they belong to the device and to the run rather than to the phase
 * machine -- the platform and the run nonce (= the login username):
 *
 *   ...,"platform":"wii","nonce":"<nonce>"}
 *
 * NO run_id: the TRANSPORT owns it. Every marker leaves through
 * mp_play_hooks.emit -> main.c's mpp_hook_emit -> hw_telemetry_emit_observer
 * (source/platform/hw_telemetry.c:2743-2747), which strips the closing brace
 * off whatever it is handed and appends `,"run_id":"<s_run_id>"}` from the
 * run identity the device itself was started with. A run_id written here
 * would be a SECOND copy -- a duplicate JSON key, and ~20 bytes on each of
 * the ~60 lines a run emits, on a transport where one lost line fails an
 * eight-minute run. chat_login_script.c:552-557 sets the precedent: its
 * marker builder writes none either.
 *
 * That also fixes the shape this function must produce: non-empty, at most
 * 2303 bytes, and ending in '}' -- anything else the observer refuses
 * outright, and the marker is silently lost.
 *
 * The body arrives from the core WITHOUT braces and may legitimately be empty
 * (then no separating comma is written). Returns false and leaves `dst` an
 * empty string when the line would not fit -- the caller must NOT emit a
 * truncated line, because a half-written JSON object is a marker the collector
 * would report as malformed rather than as missing. */
static inline bool mpp_map_marker_line(char* dst, size_t cap,
									   const struct mpp_marker* m,
									   const char* nonce,
									   const char* platform) {
	char nonce_esc[200], plat_esc[32], extra[288];
	int n;
	if(!dst || !cap)
		return false;
	dst[0] = 0;
	if(!m)
		return false;
	extra[0] = 0;
	if(strcmp(m->key, "session") == 0) {
		mpp_map_json_escape(nonce_esc, sizeof(nonce_esc), nonce ? nonce : "");
		/* `platform` is a PARAMETER so the caller can pass the very word the
		 * login half of this run stamps p0.session with (CHAT_LOGIN_PLATFORM)
		 * -- one definition in the tree, so a run can never report two
		 * platforms. Escaped and BOUNDED like the other two: it is a caller
		 * string, and `extra` must be provably big enough for all three. */
		mpp_map_json_escape(plat_esc, sizeof(plat_esc),
							platform ? platform : "");
		snprintf(extra, sizeof(extra), ",\"platform\":\"%s\",\"nonce\":\"%s\"",
				 plat_esc, nonce_esc);
	}
	n = snprintf(dst, cap,
				 "{\"event\":\"mp_play\",\"seq\":%u,\"marker\":\"%s\"%s%s%s}",
				 (unsigned)m->seq, m->key, m->body[0] ? "," : "", m->body,
				 extra);
	if(n <= 0 || (size_t)n >= cap) {
		dst[0] = 0;
		return false;
	}
	return true;
}

/* ---- 5. body ------------------------------------------------------------- */

/* The SIMULATION's own motion cutoff: entity_local_player.c:592-594 zeroes any
 * velocity axis under this before it moves the body (the Beta
 * Entity.moveEntity motion cutoff [740c583]), and entity_item.c uses the same
 * number for the same reason. It is the HORIZONTAL answer only -- see below. */
#define MPP_MAP_REST_EPS 0.005F

/* What entity_local_player.c leaves on vel[1] of a body STANDING STILL on the
 * ground, and therefore what "vertically at rest" reads as at the only point
 * this driver ever looks (mp_play_frame, once per rendered frame, after the
 * tick): (0 - 0.08) * 0.98. The tick's tail applies that step of Beta gravity
 * unconditionally (entity_local_player.c:801 and :811, the non-water/lava
 * branch) AFTER the collision move has zeroed vel[1] against the floor
 * (entity.c:610), so the cutoff above is never reached on this axis and the
 * start-of-tick clamp cannot help -- it runs before the move, on this same
 * value.
 *
 * Comparing vel[1] against MPP_MAP_REST_EPS, as this predicate did until
 * 2026-09-21, therefore made `at_rest` UNSATISFIABLE for any grounded body:
 * the first PC run skipped all twelve build cycles "never at rest in one
 * column". */
#define MPP_MAP_GROUND_VY (-0.0784F)

/* "The simulation has already stopped me" -- the one property the dig leg and
 * the pre-disconnect snapshot need: a body that will not drift into the cell it
 * is about to remove, and whose position will still be the reported one a
 * moment later.
 *
 * HORIZONTALLY that is the cutoff, because friction really does drive vx/vz
 * through it to zero. VERTICALLY it is "not rising, and falling no faster than
 * the single tick of gravity every grounded body carries": a step-down hands
 * back vel[1] = -0.6 (entity_local_player.c:725, -0.6664 after the tail) and a
 * jump +0.42 (:658), and both are refused while `on_ground` is still true.
 *
 * Every comparison is written so a NaN falls out FALSE rather than comparing
 * true -- a body whose velocity has gone bad is the last one to call at rest. */
static inline bool mpp_map_body_at_rest(bool on_ground, float vx, float vy,
										float vz) {
	if(!on_ground)
		return false;
	if(!(fabsf(vx) < MPP_MAP_REST_EPS) || !(fabsf(vz) < MPP_MAP_REST_EPS))
		return false;
	return vy <= MPP_MAP_REST_EPS
		&& vy >= MPP_MAP_GROUND_VY - MPP_MAP_REST_EPS;
}

/* TWO HEALTH SCALES MEET HERE, and the marker was speaking the wrong one.
 *
 * struct entity::health is CaveWii's INTERNAL scale: HEALTH_PER_HEART (16)
 * units per heart, MAX_PLAYER_HEALTH 160 for the ten-heart player
 * (source/network/server_interface.h:30-31, and the same 16 again as
 * COMBAT_HEALTH_PER_HEART in source/entity/combat.h:43). Beta's own scale --
 * what the wire carries, what EntityPlayer's ctor sets to 20, and what the
 * fixture prints as `ev=health h=<n>` -- is HALF-HEARTS, 0..20.
 *
 * Run pc_mp_play_20260921_143758 is the whole argument: its `ready` marker said
 * health 152 while the fixture receipt beside it said h=19. Same body, same
 * instant, two numbers -- because 19 * (16/2) = 152. A reader comparing the two
 * halves of that run's evidence had no way to know they agreed.
 *
 * ROUNDING UP, not truncating, and the reason is the death gate: the core ends
 * a run on `health <= 0`, so this must report 0 if and ONLY if the body really
 * is at or below zero. Truncation would report a body on 1..7 internal units --
 * alive, one hit from death -- as 0 and end the run on a death that never
 * happened. Every value the wire can actually produce is a multiple of 16/2
 * anyway (health_wire_to_internal multiplies by exactly that), so the rounding
 * is exact on all of them and the round trip is the identity for 0..20; it only
 * ever matters for an internal value local damage produced off the wire grid. */
#define MPP_MAP_HEALTH_PER_HEART 16 /* pinned to HEALTH_PER_HEART in the .c */
#define MPP_MAP_BETA_MAX_HEALTH 20	/* EntityPlayer ctor: this.health = 20 */

static inline int mpp_map_health_to_beta(int internal) {
	const int per_point = MPP_MAP_HEALTH_PER_HEART / 2;
	int beta;
	if(internal <= 0)
		return 0;
	beta = (internal + per_point - 1) / per_point;
	return beta > MPP_MAP_BETA_MAX_HEALTH ? MPP_MAP_BETA_MAX_HEALTH : beta;
}

/* ---- 6. yaw -------------------------------------------------------------- */

/* TWO YAW CONVENTIONS MEET HERE, and they are not the same one.
 *
 *   THE CORE speaks BETA's (stated in mp_play_core.h's BUILD_REST): yaw 0 is
 *   +Z, 90 is -X, so forward = (-sin yaw, cos yaw). That is Beta's own
 *   moveFlying [740c583 Entity.moveFlying: motionX += strafe*cos - forward*sin,
 *   motionZ += forward*cos + strafe*sin], read with strafe 0, forward 1.
 *
 *   THIS ENGINE speaks the mirrored one. entity_local_player.c:644-645 is
 *       dx = (forward * sinf(orient[0]) - strafe * cosf(orient[0])) / distf;
 *       dy = (strafe  * sinf(orient[0]) + forward * cosf(orient[0])) / distf;
 *   with dx added to vel[0] (X) and dy to vel[2] (Z) -- so with strafe 0 and
 *   forward 1 this engine's forward is (+sin yaw, cos yaw). The Z component is
 *   even and the X component is odd, so the two agree exactly when the engine
 *   yaw is the NEGATED Beta yaw.
 *
 * The whole chain is one angle: autopilot_aim_set_deg writes
 * gstate.autopilot.yaw (radians), camera_attach copies it to c->rx,
 * camera_attach_owned writes e->orient[0] = c->rx (camera.c:418), and the
 * movement code above reads e->orient[0]. So converting once, here, is enough
 * -- and the core never trusts this function anyway: it watches the OBSERVED
 * fwd_x/fwd_z and skips the leg with "aim never faced the column centre"
 * rather than walking the wrong way. */
static inline int mpp_map_engine_yaw_deg(int core_yaw_deg) {
	int d = -core_yaw_deg % 360;
	return d < 0 ? d + 360 : d;
}

/* The engine's own forward vector for a yaw ALREADY in engine convention and
 * in RADIANS -- i.e. exactly entity_local_player.c:644-645 with forward 1 and
 * strafe 0. This is what fills struct mpp_obs's fwd_x / fwd_z, read from the
 * body's live orient[0] rather than from what the driver asked for, which is
 * the point: it reports what the movement code will really do THIS frame. */
static inline void mpp_map_forward_of(float engine_yaw_rad, float* fx,
									  float* fz) {
	if(fx)
		*fx = sinf(engine_yaw_rad);
	if(fz)
		*fz = cosf(engine_yaw_rad);
}

/* ---- 7. window action ids ------------------------------------------------ */

/* THE ID OF THE CLICK THAT WAS JUST SENT, from the counter that names the NEXT
 * one. struct mpp_obs::last_click_action_id is documented as "action id of the
 * last click this client sent", and that is what the core matches a server
 * Transaction against -- but window_container.c hands out the PRE-increment
 * value (`*action_id = wc->next_action_id; wc->next_action_id++;`, :126-127
 * and :223-224), so the counter read after a click is that click's id PLUS
 * ONE. Reporting the counter raw makes the core wait for a transaction id the
 * server has never seen.
 *
 * Before any click there IS no last id, and 0 is a real id (the counter starts
 * at 0 and the first click uses it), so "none" has to be -1 -- which no real
 * id equals, so the core's "the counter moved off the baseline" test still
 * fires on the first click and can never match a transaction by accident.
 *
 * A rolled-back click is handled by construction: windowc_cancel_unsent
 * (:239) rewinds the counter TO the cancelled id, so this still names the last
 * id that really went out. */
static inline int16_t mpp_map_last_click_id(uint16_t next_action_id) {
	return next_action_id == 0u ? (int16_t)-1
								: (int16_t)(next_action_id - 1u);
}

#endif
