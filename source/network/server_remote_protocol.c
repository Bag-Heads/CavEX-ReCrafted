/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "../entity/entity_painting.h" // painting_art_by_title (MP_EVENT_ENTITY_PAINTING)
#include "mp_entity_flags.h" // animation-parity pass: pure metadata byte0 decode
#include "server_interface.h" // health_wire_to_internal (Packet8 scale seam)
#include "server_remote_internal.h"
#include "../game/achievements/game_event.h" // 0xC8 -> GE_REMOTE_STAT
#include "../graphics/render_lightning.h" // 0x47 client render bolt

/* S5.3 hunt PROBE (temporary, ROUND 10 DISCRIMINATOR): single knob for the
 * MP_EVENT_HANDSHAKE case's deferred-login hold below. 0 = effectively
 * immediate (fires on the very next server_remote_pump() poll, matching the
 * pre-round-10 inline-send timing to within one frame); 3000 = the
 * coordinator's ~3s hold that moves the login round-trip's worker-thread
 * concurrency spike from frame 4 to frame ~180, to test whether the
 * mp_connect_smoke wall tracks WHEN that spike happens rather than WHAT
 * main is executing. Held at 0 while cycle 1 isolates the cw_cond_trace
 * no-op (thread.c) in a clean, non-delayed run. */
#define CW_LOGIN_DELAY_MS 0

#ifdef CAVEWII_MP_CLIENT

#ifdef PLATFORM_WII
#include <stdio.h>
// S5.3 hunt PROBE (temporary, round 2): one-shot marker at the TOP of
// process_event (main thread, called from server_remote_pump) for the first
// 3 events -- discriminates "the event reached dispatch and process_event
// itself is where things die" from "the event never reached dispatch at
// all" (queue/pump-side death, which round 1's pre_pump/post_pump markers
// already argued against, but this pins it precisely against the
// handshake-reply -> queue_frame(login) -> thread_cond_signal handoff that
// is the current leading suspect). Main-thread-only writes.
static void cw_dispatch_trace(int kind, int packet_type) {
	// ROUND 9 (coordinator plan item 1): widened 6 -> 60. round 8 proved the
	// current wall is state-deterministic (byte-identical trace across a
	// budget change AND a binary change), so the prime suspect is now
	// server_remote_pump's drain loop actually reaching MP_EVENT_LOGIN --
	// which, if the read thread has queued multiple events by the time a
	// later frame's pump() runs, could fire this several times in ONE call.
	// 6 was sized for "at most one real event per frame" (true so far); no
	// longer assumed.
	static unsigned budget = 60;
	if(budget == 0 || CW_SD_TRACE_OFF())
		return;
	budget--;
	FILE* f = fopen("sd:/cw_boot_trace.txt", "a");
	if(!f)
		return;
	fprintf(f, "process_event kind=%d ptype=%d\n", kind, packet_type);
	fflush(f);
	fclose(f);
}
// S5.3 hunt PROBE (temporary, round 2 cont.): take 10's evidence narrowed
// the death to WITHIN (or immediately after) process_event's MP_EVENT_LOGIN
// case -- the condvar mechanism and the read thread's per-packet lock
// region are now BOTH proven clean across all 3 decoded events, and the
// dispatch marker fired for kind=1 ptype=1 (LOGIN) with nothing after it.
// This case is the FIRST place a remote MP session calls worldgen_create()
// SYNCHRONOUSLY on the MAIN/render thread (singleplayer creates worlds on
// the dedicated server thread instead) -- a call-depth/stack combination
// that has never run on Wii before, and this codebase has a documented
// precedent for exactly this failure class (boot-death-bisect: a 188KB
// stack frame silently exceeding a 128KB budget). Own small budget, bare
// label only (no per-call values needed -- presence/absence of each line
// is the whole signal).
// S5.3 hunt PROBE (temporary, round 4): main-thread stack watermark. The
// take-15 death sits AFTER the deep world-install chain RETURNS cleanly and
// before the next function's first line -- the classic delayed-victim shape
// of a stack overflow at peak depth (boot-death precedent: ogc/tuxedo main
// stack is 128KB). Paint the unused stack below SP before the deep call,
// scan for the low-water mark after it returns, log bytes-left. Main-thread
// only; paints strictly below SP-512 and above a conservative floor.
extern void* __ppc_main_sp; /* tuxedo runtime: main stack TOP (variable) */
#define CW_STACK_PAINT 0x5AFE5AFEu
#define CW_MAIN_STACK_BYTES (128u * 1024u)
static uint32_t* cw_paint_floor;
static void cw_stack_paint(void) {
	uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
	uintptr_t floor_est
		= (uintptr_t)__ppc_main_sp - CW_MAIN_STACK_BYTES + 4096u;
	uintptr_t lo = (floor_est + 3u) & ~3u;
	uintptr_t hi = (sp - 512u) & ~3u;
	cw_paint_floor = NULL;
	if(hi <= lo)
		return;
	for(uint32_t* p = (uint32_t*)lo; p < (uint32_t*)hi; p++)
		*p = CW_STACK_PAINT;
	cw_paint_floor = (uint32_t*)lo;
	if(CW_SD_TRACE_OFF())
		return;
	FILE* f = fopen("sd:/cw_boot_trace.txt", "a");
	if(f) {
		fprintf(f, "stack_paint sp=%08lx top=%08lx painted=%lu\n",
				(unsigned long)sp, (unsigned long)(uintptr_t)__ppc_main_sp,
				(unsigned long)(hi - lo));
		fclose(f);
	}
}
static void cw_stack_scan(const char* label) {
	if(!cw_paint_floor || CW_SD_TRACE_OFF())
		return;
	uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
	uint32_t* p = cw_paint_floor;
	while((uintptr_t)p < sp && *p == CW_STACK_PAINT)
		p++;
	unsigned long left
		= (unsigned long)((uintptr_t)p - (uintptr_t)cw_paint_floor) + 4096u;
	FILE* f = fopen("sd:/cw_boot_trace.txt", "a");
	if(f) {
		fprintf(f, "%s stack_left=%lu low=%08lx\n", label, left,
				(unsigned long)(uintptr_t)p);
		fclose(f);
	}
}

static void cw_dispatch_trace_bare(const char* label) {
	static unsigned budget = 10;
	if(budget == 0 || CW_SD_TRACE_OFF())
		return;
	budget--;
	FILE* f = fopen("sd:/cw_boot_trace.txt", "a");
	if(!f)
		return;
	fprintf(f, "%s\n", label);
	fflush(f);
	fclose(f);
}
#endif

/* The process_event S->C packet dispatch (handshake/login/play handlers)
 * and the remote_process_srpc S2 C->S dispatch, minus the entity and chunk
 * domains (server_remote_entities.c / server_remote_chunks.c own those --
 * process_event still calls into them through server_remote_internal.h's
 * cross-module declarations). Split out of the former monolithic
 * server_remote.c (2026-08-26, maintainer-register item 1); see
 * server_remote_internal.h's top comment. process_event and
 * remote_process_srpc are declared non-static in server_remote_internal.h
 * (server_remote.c's pump calls both); everything else here stays static. */


/* CaveWii's enum side ordinals (blocks_data.h: TOP=0,BOTTOM=1,LEFT=2,
 * RIGHT=3,FRONT=4,BACK=5) do NOT match Beta's wire face byte. Confirmed via
 * the axis deltas in ItemBlock.onItemUse: side 0 decrements y (DOWN), 1
 * increments y (UP), 2 decrements z (NORTH), 3 increments z (SOUTH), 4
 * decrements x (WEST), 5 increments x (EAST) [740c583 ItemBlock.onItemUse].
 * blocks_side_offset (blocks.c) gives CaveWii's own axis mapping (TOP=+Y,
 * BOTTOM=-Y, LEFT=-X, RIGHT=+X, BACK=+Z, FRONT=-Z); this table converts
 * between the two by matching axis deltas, not by casting the ordinal. */
static uint8_t remote_side_to_face(enum side side) {
	switch(side) {
		case SIDE_BOTTOM: return 0; /* -Y */
		case SIDE_TOP: return 1;	/* +Y */
		case SIDE_FRONT: return 2;  /* -Z */
		case SIDE_BACK: return 3;   /* +Z */
		case SIDE_LEFT: return 4;   /* -X */
		case SIDE_RIGHT: return 5;  /* +X */
		default:
			/* enum side is engine-internal, never wire data -- this should
			 * be unreachable. Catch a future enum-side addition loudly in
			 * asserts-on builds; the shipping .dol still needs a defined,
			 * safe fallback since assert() compiles out under NDEBUG. */
			assert(false && "remote_side_to_face: unhandled enum side");
			return 1; /* +Y: safe NDEBUG fallback, matches prior behaviour */
	}
}

static struct mp_item_stack mp_item_from_data(bool has, const struct item_data* item) {
	if(!has || !item)
		return (struct mp_item_stack) {.present = false};
	return (struct mp_item_stack) {.present = true, .id = (int16_t)item->id,
		.count = item->count, .damage = (int16_t)item->durability};
}

static struct item_data item_data_from_mp(const struct mp_item_stack* item) {
	if(!item || !item->present)
		return (struct item_data) {0};
	return (struct item_data) {.id = (uint16_t)item->id,
		.durability = (uint16_t)item->damage, .count = item->count};
}

/* --- push-0921/B2 observation seams (recording only) ---------------------
 *
 * design-mp-play-10m.md section 6 (a)/(b), Astra gate 1 Lane B item 3. Each
 * of these three is called from process_event BESIDE an effect that has
 * already happened -- they read nothing the handler did not already hold,
 * write nothing the handler reads back, and take no lock (process_event is
 * the pump thread, the sole writer, exactly like chat_lines_received). The
 * full contract for the rings lives on struct server_remote_status
 * (server_remote.h); the one rule worth repeating here is that the serial is
 * assigned FIRST and the record is written at `serial % capacity`, so the
 * ring always holds the newest <capacity> records.
 *
 * Saturation: a serial is a receipt identity, so it saturates rather than
 * wrapping to 0 -- a reader must never see one go backwards, and a wrapped 0
 * is indistinguishable from "never written". Unreachable in practice (2^32
 * records), but free to state. */
void remote_record_block_echo(struct server_remote_impl* remote, int32_t x,
							  uint8_t y, int32_t z, uint8_t type,
							  uint8_t metadata) {
	if(remote->block_echo_serial == UINT32_MAX)
		return;
	uint32_t serial = ++remote->block_echo_serial;
	struct server_remote_block_echo record
		= (struct server_remote_block_echo) {.x = x, .y = (int32_t)y, .z = z,
			.type = type, .metadata = metadata, .serial = serial};
	remote->block_echoes[serial % SERVER_REMOTE_BLOCK_ECHO_RING] = record;
	/* push-0921/B19: THE WATCHED CELL gets the SAME record, in a slot the ring
	 * cannot evict. One assignment, not a second reading of the packet, so the
	 * serial the core finds in the slot is by construction the serial the ring
	 * entry got -- the core compares both against one baseline.
	 *
	 * This is the ONE place a per-cell block echo is recorded: both the single
	 * MP_EVENT_BLOCK_CHANGE and every cell of MP_EVENT_MULTI_BLOCK_CHANGE come
	 * through here, so the slot cannot miss a shape the ring catches.
	 *
	 * `watch_on` is false unless a script armed it (server_remote_play_watch),
	 * so for every other profile and for normal play this is one predictable
	 * branch and nothing else. */
	if(remote->watch_on && remote->watch_x == x
	   && remote->watch_y == (int32_t)y && remote->watch_z == z) {
		remote->watch_echo = record;
		/* hw1030g: and in the cell's own history, so a LATER record for the
		 * same cell (water flowing into a dug hole, a resend) cannot mask the
		 * one the script is waiting for. Same record, same serial. */
		remote->watch_history[remote->watch_history_n
							  % SERVER_REMOTE_WATCH_HISTORY]
			= record;
		if(remote->watch_history_n != UINT32_MAX)
			remote->watch_history_n++;
	}
}

static void remote_record_txn_result(struct server_remote_impl* remote,
									 int8_t window, int16_t action_id,
									 bool accepted) {
	if(remote->txn_serial == UINT32_MAX)
		return;
	uint32_t serial = ++remote->txn_serial;
	remote->txn_results[serial % SERVER_REMOTE_TXN_RING]
		= (struct server_remote_txn_result) {.window = window,
			.action_id = action_id, .accepted = accepted, .serial = serial};
}

/* `item` NULL records the whole-window-refresh form (slot -1, item -1); an
 * absent stack records item -1 with a zero count/damage. */
static void remote_record_set_slot(struct server_remote_impl* remote,
								   int8_t window, int16_t slot,
								   const struct mp_item_stack* item) {
	if(remote->set_slot_serial == UINT32_MAX)
		return;
	uint32_t serial = ++remote->set_slot_serial;
	bool present = item && item->present;
	remote->set_slots[serial % SERVER_REMOTE_SET_SLOT_RING]
		= (struct server_remote_set_slot) {.window = window, .slot = slot,
			.item = present ? item->id : (int16_t)-1,
			.count = present ? item->count : (uint8_t)0,
			.damage = present ? item->damage : (int16_t)0, .serial = serial};
}

static void remote_flush_sign_edit(struct server_remote_impl* remote) {
	thread_mutex_lock(&remote->lock);
	if(!remote->sign_edit.active) {
		thread_mutex_unlock(&remote->lock);
		return;
	}
	int32_t x = remote->sign_edit.x;
	int16_t y = remote->sign_edit.y;
	int32_t z = remote->sign_edit.z;
	char line_buf[MP_SIGN_LINE_COUNT][SIGN_EDIT_COLS + 1];
	memcpy(line_buf, remote->sign_edit.lines, sizeof(line_buf));
	memset(&remote->sign_edit, 0, sizeof(remote->sign_edit));
	thread_mutex_unlock(&remote->lock);

	/* Beta's line is a variable-length String with no fixed-width padding;
	 * trim the local 16-col grid's trailing spaces and cap to the wire's
	 * 15-code-unit maximum. [740c583 Packet130UpdateSign.readPacketData] */
	const char* lines[MP_SIGN_LINE_COUNT];
	for(size_t i = 0; i < MP_SIGN_LINE_COUNT; i++) {
		size_t len = strlen(line_buf[i]);
		while(len > 0 && line_buf[i][len - 1] == ' ')
			len--;
		if(len > MP_SIGN_LINE_MAX)
			len = MP_SIGN_LINE_MAX;
		line_buf[i][len] = '\0';
		lines[i] = line_buf[i];
	}
	struct mp_frame frame;
	if(mp_encode_update_sign(&frame, x, y, z, lines))
		queue_frame_playing(remote, &frame);
}

/* P3 MP-MISC: arm the online sign editor for our own Packet15. Beta's
 * ItemSign.onItemUse: face 0 (bottom) never places; faces 1..5 offset the
 * clicked cell (+y, -z, +z, -x, +x) to the cell the sign fills. Any other
 * held item (or a frame that was not queued) disarms. [740c583
 * ItemSign.onItemUse] */
static void remote_arm_sign_place(struct server_remote_impl* remote,
	bool has_item, int16_t item_id, int32_t x, int32_t y, int32_t z,
	uint8_t face) {
	bool sign = has_item && item_id == ITEM_SIGN && face >= 1 && face <= 5;
	if(sign) {
		if(face == 1) y++;
		if(face == 2) z--;
		if(face == 3) z++;
		if(face == 4) x--;
		if(face == 5) x++;
	}
	remote->sign_place_pending.armed = sign && y >= 0 && y < WORLD_HEIGHT;
	remote->sign_place_pending.x = x;
	remote->sign_place_pending.y = (int16_t)y;
	remote->sign_place_pending.z = z;
}

/* The server's Packet53 for the armed cell decides: a sign (63 post / 68
 * wall) means our placement landed and the server made an editable
 * TileEntitySign, so open the editor as Beta's client does after its local
 * placement (empty lines; GuiEditSign.onGuiClosed sends Packet130 on close).
 * Anything else there (placement refused, cell occupied) disarms with no
 * editor -- the same Packet53 that reverts Beta's own predicted sign.
 * Deviation (Bucket B, feel): the editor opens one server round trip after
 * the click instead of on the click, because this client does not predict
 * placements. Returns true when the editor opened. */
static bool remote_sign_place_echo(struct server_remote_impl* remote,
	int32_t x, int32_t y, int32_t z, uint8_t block_type) {
	if(!remote->sign_place_pending.armed || remote->sign_place_pending.x != x
	   || remote->sign_place_pending.y != y || remote->sign_place_pending.z != z)
		return false;
	remote->sign_place_pending.armed = false;
	if(block_type != BLOCK_SIGN_POST && block_type != BLOCK_SIGN)
		return false;
	thread_mutex_lock(&remote->lock);
	memset(&remote->sign_edit, 0, sizeof(remote->sign_edit));
	remote->sign_edit.active = true;
	remote->sign_edit.window = REMOTE_SIGN_EDIT_WINDOW;
	remote->sign_edit.x = x;
	remote->sign_edit.y = (int16_t)y;
	remote->sign_edit.z = z;
	for(size_t i = 0; i < MP_SIGN_LINE_COUNT; i++)
		memset(remote->sign_edit.lines[i], ' ', SIGN_EDIT_COLS);
	thread_mutex_unlock(&remote->lock);
	remote->sign_editor_opened++;
	struct client_rpc call = {
		.type = CRPC_OPEN_WINDOW,
		.payload.window_open = {
			.window = REMOTE_SIGN_EDIT_WINDOW,
			.type = WINDOW_TYPE_SIGN,
			.slot_count = (uint8_t)(MP_SIGN_LINE_COUNT * SIGN_EDIT_COLS),
		},
	};
	clin_process_direct_remote(&call);
	return true;
}

void remote_process_srpc(struct server_rpc* call, void* user) {
	struct server_remote* remote = user;
	struct server_remote_impl* impl = remote->impl;
	if(call->type == SRPC_UNLOAD_WORLD) {
		(void)server_remote_disconnect(remote, "Quitting");
		return;
	}
	/* Fast-path advisory only, NOT the correctness gate: a stale read here is
	 * harmless because every case below that actually sends a frame commits
	 * through queue_frame_playing/queue_movement_playing (Fix B,
	 * server_remote_transport.c), which re-validates REMOTE_PLAY under the
	 * SAME lock acquisition as the queue append. This early check exists
	 * purely to skip building a frame for an already-dead session and to
	 * gate the few cases (SRPC_SIGN_SET) that mutate local state without
	 * ever reaching a queue_*_playing call. */
	thread_mutex_lock(&impl->lock);
	bool playing = impl->state == REMOTE_PLAY && !impl->stop_now;
	thread_mutex_unlock(&impl->lock);
	if(!playing)
		return; /* No wire-legal S2 action exists outside REMOTE_PLAY. */

	switch(call->type) {
		case SRPC_PLAYER_POS: {
			thread_mutex_lock(&impl->lock);
			bool correction_current = call->payload.player_pos.correction_seq
				== impl->correction_seq;
			/* "The remote world install has not completed yet", i.e. the
			 * client is still behind its GuiDownloadTerrain-equivalent hold
			 * (server_remote_chunks.c's remote_emit_load_completion_locked
			 * is what sets this). See the pre-world force below. */
			bool preworld = !impl->remote_load_complete_sent;
			thread_mutex_unlock(&impl->lock);
			if(!correction_current)
				return; /* Stale correction samples cannot cross this boundary. */

			/* Strict comparisons and variant order match
			 * EntityClientPlayerMP.sendMotionUpdates exactly: position+look
			 * -> 13, position -> 11, look -> 12, neither -> 10. No epsilon
			 * is present in Beta. [740c583
			 * EntityClientPlayerMP.sendMotionUpdates]
			 *
			 * `preworld` DEVIATION (documented, bounded, load-bearing): while
			 * the world is still streaming in, always emit the POSITION-
			 * bearing variant even when the sample is byte-identical to the
			 * last one. Beta never reaches this state -- handleFlying flips
			 * doneLoadingTerrain on the FIRST server position packet [740c583
			 * NetClientHandler.handleFlying:261-267], so a vanilla client is
			 * "loaded" from that instant and its stationary Packet10Flying
			 * costs it nothing. CaveWii deliberately holds physics behind the
			 * load screen until the destination column actually installs
			 * (7ec5536, the sky-respawn free-fall fix), which parks the player
			 * perfectly still -- and a still player dedupes down to
			 * Packet10Flying, which carries no position. mc173 only re-streams
			 * chunks from a position-BEARING packet (player.rs
			 * handle_position_look_inner: `if pos.is_some() {
			 * self.update_chunks(sw) }`; update_chunks itself skips columns
			 * the server has not loaded yet and does NOT mark them tracked,
			 * so they are strictly re-offer-able), and otherwise only at
			 * handle_player_join. So without this, a client whose spawn column
			 * was not resident server-side at join time can never be sent it:
			 * the hold waits for a column the server will never re-offer.
			 * Costs one ~34-byte packet per 50ms echo, ONLY inside the
			 * pre-world window, and stops the instant the install completes. */
			bool moved = preworld || !impl->have_last_sent_position
				|| call->payload.player_pos.x != impl->last_sent_x
				|| call->payload.player_pos.y != impl->last_sent_y
				|| call->payload.player_pos.z != impl->last_sent_z;
			bool looked = !impl->have_last_sent_look
				|| call->payload.player_pos.rx != impl->last_sent_yaw
				|| call->payload.player_pos.ry != impl->last_sent_pitch;
			if(impl->mounted) {
				/* Riding [740c583 EntityClientPlayerMP.sendMotionUpdates,
				 * `ridingEntity != null` branch]: the client never reports its
				 * real position; every tick it sends its rider MOTION in the
				 * x/z slots with -999 for y and stance -- Packet11 when the
				 * look changed, else Packet13 with the look (Beta's literal,
				 * inverted-looking choice, reproduced as-is). The server takes
				 * yaw/pitch from the 13 and drives the vehicle from the motion.
				 * The last-sent POSITION is not updated (Beta's flag1 = false),
				 * so the first sample after a dismount is always sent. */
				struct mp_movement riding = {
					.x = call->payload.player_pos.vel_x,
					.y = -999.0,
					.stance = -999.0,
					.z = call->payload.player_pos.vel_z,
					.yaw = call->payload.player_pos.rx,
					.pitch = call->payload.player_pos.ry,
					.on_ground = call->payload.player_pos.on_ground,
				};
				enum mp_event_type riding_type
					= looked ? MP_EVENT_PLAYER_POSITION : MP_EVENT_PLAYER_LOOK_MOVE;
				if(!queue_movement_playing(impl, &riding, riding_type))
					return;
				if(looked) {
					impl->last_sent_yaw = call->payload.player_pos.rx;
					impl->last_sent_pitch = call->payload.player_pos.ry;
					impl->have_last_sent_look = true;
				}
				return;
			}
			struct mp_movement movement = {
				.x = call->payload.player_pos.x,
				/* CaveWii's player Y is Beta posY (eye reference). Packet11/13
				 * send bounding-box minY first and posY as stance. */
				.y = call->payload.player_pos.y - 1.62,
				.stance = call->payload.player_pos.y,
				.z = call->payload.player_pos.z,
				.yaw = call->payload.player_pos.rx,
				.pitch = call->payload.player_pos.ry,
				.on_ground = call->payload.player_pos.on_ground,
			};
			enum mp_event_type type = moved
				? (looked ? MP_EVENT_PLAYER_LOOK_MOVE : MP_EVENT_PLAYER_POSITION)
				: (looked ? MP_EVENT_PLAYER_LOOK : MP_EVENT_FLYING);
			if(!queue_movement_playing(impl, &movement, type))
				return;
			if(moved) {
				impl->last_sent_x = call->payload.player_pos.x;
				impl->last_sent_y = call->payload.player_pos.y;
				impl->last_sent_z = call->payload.player_pos.z;
				impl->have_last_sent_position = true;
				/* F7: THE ADMISSION CENTRE FOLLOWS THE POSITION WE SEND.
				 * Beta's server emits a position packet only at login,
				 * respawn, teleport or a rubber-band; during ordinary walking
				 * it ACCEPTS the client's Packet10-13 and streams chunks
				 * around that CLIENT-REPORTED position [740c583
				 * NetServerHandler.handleFlying -> EntityPlayerMP position;
				 * ServerConfigurationManager -> PlayerManager
				 * updateMountedMovingPlayer]. So the position we last SENT is
				 * the server's view of the player, and it is the only honest
				 * admission centre while walking. Before this, the centre had
				 * exactly two feeders -- an INCOMING correction
				 * (remote_handle_correction) and the Packet6 spawn hint -- so
				 * nothing moved it at all between teleports: real-Wii run
				 * dense_walk_0913c walked 53 blocks, mp_backed_cols reached
				 * 403, and mp_resident_cols never left the 81 columns around
				 * the LOGIN centre with mp_demotions at 0. Every new column
				 * arrived BACKED and nothing ever promoted, because the
				 * radius gate was still measuring from the spawn.
				 *
				 * `authoritative` = true: this IS a real position, not the
				 * weaker spawn hint, so a Packet6 can never drag the centre
				 * back -- and a later INCOMING correction still overrides it
				 * through this same setter (remote_handle_correction writes
				 * after us, from process_event on this same pump thread), so
				 * a rubber-band or teleport wins as it always did.
				 *
				 * Placed after queue_movement_playing returned true, so it is
				 * reached only when the frame was actually appended under that
				 * function's REMOTE_PLAY re-check held across the append
				 * (server_remote_transport.c queue_movement_playing) -- i.e.
				 * never before login. No lock and no new critical section:
				 * the centre is pump-thread-EXCLUSIVE for writes
				 * (server_remote_internal.h beside admit_centre_x), exactly
				 * like the last_sent_* fields written immediately above and
				 * like remote_handle_correction's own call, and
				 * remote_process_srpc runs on the pump thread
				 * (server_remote.c's svin_process_messages). Wire bytes are
				 * untouched: nothing is added to any packet and the send
				 * order/cadence above is unchanged.
				 *
				 * Not the mounted branch: its Packet11/13 carry rider MOTION
				 * in the x/z slots with y/stance = -999 [740c583
				 * EntityClientPlayerMP.sendMotionUpdates, `ridingEntity !=
				 * null`], which is not a position at all -- that branch
				 * returns above, before this point, and the vehicle's server
				 * corrections keep feeding the centre instead. Not Packet10 or
				 * Packet12 either: `moved` is false for those and they carry
				 * no position. */
				remote_admission_centre_set(impl, call->payload.player_pos.x,
											call->payload.player_pos.z, true);
			}
			if(looked) {
				impl->last_sent_yaw = call->payload.player_pos.rx;
				impl->last_sent_pitch = call->payload.player_pos.ry;
				impl->have_last_sent_look = true;
			}
			return;
		}
		case SRPC_PLAYER_STATE: {
			/* Reliable sneak-edge snapshot; on_ground rides SRPC_PLAYER_POS's
			 * own Flying/Position/Look variants instead of a second wire
			 * write here. [740c583 EntityClientPlayerMP.sendMotionUpdates] */
			/* MP-BED: a new leave_bed_seq is one GuiSleepMP "Leave Bed" press:
			 * "var1.addToSendQueue(new Packet19EntityAction(this.mc.thePlayer,
			 * 3));" (GuiSleepMP.java:45-49). The server answers with its own
			 * wakeUpPlayer (NetServerHandler.java:452-454). Sent once per press,
			 * independent of the sneak edge below. */
			uint32_t leave_seq = call->payload.player_state.leave_bed_seq;
			/* Only while the server still has us in bed: GuiSleepMP exists
			 * only while isPlayerSleeping (Minecraft.java:963-967), so a press
			 * that raced the server's wake (Packet18 animate 3 already here)
			 * is consumed without a send. */
			if(leave_seq != 0 && leave_seq != impl->leave_bed_seq_sent
			   && !impl->local_sleeping)
				impl->leave_bed_seq_sent = leave_seq;
			if(leave_seq != 0 && leave_seq != impl->leave_bed_seq_sent) {
				thread_mutex_lock(&impl->lock);
				int32_t sleeper_id = impl->entity_id;
				thread_mutex_unlock(&impl->lock);
				struct mp_frame leave;
				if(mp_encode_entity_action(&leave, sleeper_id, 3)
				   && queue_frame_playing(impl, &leave)) {
					impl->leave_bed_seq_sent = leave_seq;
					impl->kind_counts[SR_KIND_TX_LEAVE_BED]++;
				}
			}
			thread_mutex_lock(&impl->lock);
			bool changed = !impl->have_last_sent_sneaking
				|| call->payload.player_state.sneaking != impl->last_sent_sneaking;
			thread_mutex_unlock(&impl->lock);
			if(!changed)
				return;
			struct mp_frame frame;
			uint8_t state = call->payload.player_state.sneaking ? 1 : 2;
			thread_mutex_lock(&impl->lock);
			int32_t local_id = impl->entity_id;
			thread_mutex_unlock(&impl->lock);
			if(mp_encode_entity_action(&frame, local_id, state)
			   && queue_frame_playing(impl, &frame)) {
				thread_mutex_lock(&impl->lock);
				impl->last_sent_sneaking = call->payload.player_state.sneaking;
				impl->have_last_sent_sneaking = true;
				thread_mutex_unlock(&impl->lock);
			}
			return;
		}
		case SRPC_HOTBAR_SLOT: {
			struct mp_frame frame;
			if(mp_encode_block_item_switch(
				   &frame, (int16_t)call->payload.hotbar_slot.slot))
				queue_frame_playing(impl, &frame);
			return;
		}
		case SRPC_BLOCK_DIG: {
			struct mp_frame frame;
			/* status 0=start, 2=finish. [740c583
			 * PlayerControllerMP.clickBlock]
			 * [740c583 PlayerControllerMP.sendBlockRemoving] */
			uint8_t status = call->payload.block_dig.finished ? 2 : 0;
			if(mp_encode_block_dig(&frame, status, call->payload.block_dig.x,
					(uint8_t)call->payload.block_dig.y,
					call->payload.block_dig.z,
					remote_side_to_face(call->payload.block_dig.side)))
				queue_frame_playing(impl, &frame);
			return;
		}
		case SRPC_BLOCK_PLACE: {
			struct mp_item_stack item = mp_item_from_data(
				call->payload.block_place.has_held_item,
				&call->payload.block_place.held_item);
			struct mp_frame frame;
			uint8_t face = remote_side_to_face(call->payload.block_place.side);
			bool sent = mp_encode_place(&frame, call->payload.block_place.x,
					(uint8_t)call->payload.block_place.y,
					call->payload.block_place.z, face, &item)
				&& queue_frame_playing(impl, &frame);
			remote_arm_sign_place(impl, sent && item.present, item.id,
				call->payload.block_place.x, call->payload.block_place.y,
				call->payload.block_place.z, face);
			return;
		}
		case SRPC_USE_ITEM: {
			struct mp_item_stack item = mp_item_from_data(
				call->payload.use_item.has_held_item,
				&call->payload.use_item.held_item);
			struct mp_frame frame;
			/* Vanilla air-use sentinel: x=y=z=-1, face=255. [740c583
			 * PlayerControllerMP.sendUseItem] */
			if(mp_encode_place(&frame, -1, 255, -1, 255, &item)
			   && queue_frame_playing(impl, &frame))
				impl->kind_counts[SR_KIND_TX_USE_ITEM]++;
			return;
		}
		case SRPC_ARM_SWING: {
			/* EntityClientPlayerMP.swingItem [EntityClientPlayerMP.java:
			 * 118-121]: Packet18Animation(this, 1) -- our own entity id,
			 * animate 1. WHEN it is sent (before the left click's Packet7/
			 * Packet14, after a successful place's Packet15) is decided by
			 * the client (game/mp_arm_swing.h); the svin FIFO carries it here
			 * in that order. */
			thread_mutex_lock(&impl->lock);
			int32_t local_id = impl->entity_id;
			thread_mutex_unlock(&impl->lock);
			struct mp_frame frame;
			if(mp_encode_animation(&frame, local_id, 1))
				queue_frame_playing(impl, &frame);
			return;
		}
		case SRPC_DROP_ITEM: {
			struct mp_frame frame;
			/* status 4, zero coordinates, no item field on the wire -- the
			 * server derives which stack drops from its OWN authoritative
			 * inventory regardless of one-vs-stack. [740c583
			 * EntityClientPlayerMP.dropCurrentItem] */
			if(mp_encode_block_dig(&frame, 4, 0, 0, 0, 0))
				queue_frame_playing(impl, &frame);
			return;
		}
		case SRPC_ENTITY_ATTACK:
		case SRPC_ENTITY_USE: {
			thread_mutex_lock(&impl->lock);
			int32_t local_id = impl->entity_id;
			thread_mutex_unlock(&impl->lock);
			uint32_t target = call->type == SRPC_ENTITY_ATTACK
				? call->payload.entity_attack.entity_id
				: call->payload.entity_use.entity_id;
			struct mp_frame frame;
			/* isLeftClick: 1 attack, 0 use/interact. [740c583
			 * PlayerControllerMP.attackEntity]
			 * [740c583 PlayerControllerMP.interactWithEntity] */
			if(mp_encode_use_entity(&frame, local_id, (int32_t)target,
					call->type == SRPC_ENTITY_ATTACK ? 1 : 0)) {
				queue_frame_playing(impl, &frame);
				/* Live-session trace (PC proof rig): the only client-side
				 * record of a Packet7 leaving -- Beta logs nothing here. */
				fprintf(stderr, "[mp] use-entity %s -> target %d (us %d)\n",
						call->type == SRPC_ENTITY_ATTACK ? "attack" : "use",
						(int)target, (int)local_id);
			}
			return;
		}
		case SRPC_RESPAWN: {
			thread_mutex_lock(&impl->lock);
			int8_t dimension = impl->dimension;
			thread_mutex_unlock(&impl->lock);
			struct mp_frame frame;
			if(mp_encode_respawn(&frame, dimension))
				queue_frame_playing(impl, &frame);
			return;
		}
		case SRPC_WINDOW_CLICK: {
			/* Beta Packet102: quick move sets shift and reports the original source.
			 * The remote host owns transfer semantics and the final slot sync. */
			struct item_data expected = call->payload.window_click.expected;
			struct mp_item_stack source = {.present = expected.id != 0,
				.id = expected.id, .count = expected.count, .damage = expected.durability};
			struct mp_frame frame;
			if(mp_encode_window_click(&frame,
					(int8_t)call->payload.window_click.window,
					(int16_t)call->payload.window_click.slot,
					call->payload.window_click.right_click ? 1 : 0,
					(int16_t)call->payload.window_click.action_id,
					call->payload.window_click.quick_move,
					&source))
				queue_frame_playing(impl, &frame);
			return;
		}
		case SRPC_WINDOW_DROP: {
			/* Container.java:48-61: outside drop always returns a null stack. */
			struct mp_frame frame;
			if(mp_encode_window_click(&frame,
				(int8_t)call->payload.window_drop.window, -999,
				call->payload.window_drop.right ? 1 : 0,
				(int16_t)call->payload.window_drop.action_id, false, NULL))
				queue_frame_playing(impl, &frame);
			return;
		}
		case SRPC_WINDOW_TRANSACTION: {
			struct mp_frame frame;
			if(mp_encode_transaction(&frame, call->payload.window_transaction.window,
				call->payload.window_transaction.action_id,
				call->payload.window_transaction.accepted))
				queue_frame_playing(impl, &frame);
			return;
		}
		case SRPC_WINDOW_CLOSE: {
			uint8_t window = call->payload.window_close.window;
			impl->active_window = 0;
			thread_mutex_lock(&impl->lock);
			bool is_sign
				= impl->sign_edit.active && impl->sign_edit.window == window;
			thread_mutex_unlock(&impl->lock);
			if(is_sign) {
				/* Beta never opens/closes a window for sign editing -- only
				 * the accumulated whole-sign Packet130 crosses the wire.
				 * [740c583 GuiEditSign.onGuiClosed] */
				remote_flush_sign_edit(impl);
				return;
			}
			struct mp_frame frame;
			if(mp_encode_close_window(&frame, (int8_t)window))
				queue_frame_playing(impl, &frame);
			return;
		}
		case SRPC_SIGN_SET: {
			/* Only the editor remote_sign_place_echo opened accepts edits:
			 * its coordinates come from the server-confirmed cell, never
			 * from the payload (the client's sign window carries no block
			 * position online, so payload x/y/z are 0 and used to aim
			 * Packet130 at 0,0,0). Beta's GuiEditSign edits locally with no
			 * round trip, so the reply is immediate; column 15 stays blank
			 * (keyTyped's `length() < 15`), like inventory_sign_set_char. */
			size_t slot = call->payload.sign_set.slot;
			uint8_t ch = (uint8_t)call->payload.sign_set.ch;
			thread_mutex_lock(&impl->lock);
			bool accept = impl->sign_edit.active
				&& impl->sign_edit.window == call->payload.sign_set.window
				&& slot < (size_t)MP_SIGN_LINE_COUNT * SIGN_EDIT_COLS
				&& (slot % SIGN_EDIT_COLS) < MP_SIGN_LINE_MAX
				&& ch >= ' ' && ch <= '~';
			if(accept)
				impl->sign_edit.lines[slot / SIGN_EDIT_COLS]
									 [slot % SIGN_EDIT_COLS] = (char)ch;
			thread_mutex_unlock(&impl->lock);
			struct client_rpc reply = {
				.type = CRPC_WINDOW_TRANSACTION,
				.payload.window_transaction = {
					.window = call->payload.sign_set.window,
					.action_id = call->payload.sign_set.action_id,
					.accepted = accept,
				},
			};
			clin_process_direct_remote(&reply);
			return;
		}
		case SRPC_CHAT_SEND: {
			/* The composer's Enter (screen_chat.c). Sanitize at the production
			 * choke point exactly like server_remote_send_chat (Beta's composer
			 * cap is 100 with \247/controls untypeable [740c583 GuiChat:40];
			 * CB-lineage servers KICK on either), then ONE Packet3Chat
			 * [740c583 EntityClientPlayerMP.sendChatMessage]. An empty
			 * sanitized result sends nothing -- Beta's Enter on a blank draft. */
			char sanitized[401];
			if(mp_chat_sanitize_outbound(call->payload.chat_send.text, sanitized,
										 sizeof(sanitized))
			   == 0)
				return;
			struct mp_frame frame;
			if(mp_encode_chat(&frame, sanitized) && queue_frame_playing(impl, &frame))
				impl->chat_lines_sent++;
			return;
		}
		default:
			return; /* Boundary-local RPCs (plan 6.2) and P2/P3 traffic. */
	}
}



static void remote_handle_correction(struct server_remote* owner,
									 const struct remote_event* event) {
	struct server_remote_impl* remote = owner->impl;
	const struct mp_movement* movement = &event->packet.payload.movement;
	if((movement->moving
		&& (!isfinite(movement->x) || !isfinite(movement->y)
			|| !isfinite(movement->stance) || !isfinite(movement->z)
			|| movement->x < INT32_MIN || movement->x > INT32_MAX
			|| movement->z < INT32_MIN || movement->z > INT32_MAX))
	   || (movement->rotating
		   && (!isfinite(movement->yaw) || !isfinite(movement->pitch)))) {
		publish_error(remote, event->generation,
			"invalid remote movement scalar");
		return;
	}
	if(movement->moving) {
		remote->authoritative_x = movement->x;
		remote->authoritative_z = movement->z;
		/* Incoming correction yPosition is installed as Entity.posY; stance is
		 * rewritten only for the echo. [740c583 NetClientHandler.handleFlying] */
		remote->authoritative_y = movement->y;
		remote->have_authoritative_position = true;
		/* F5b (review findings H5/L12) + F7: EVERY server position -- the
		 * login teleport, an ordinary correction, the position that follows a
		 * same-dimension respawn -- AND every position the client SENDS
		 * (remote_process_srpc's SRPC_PLAYER_POS, the Packet11/13 above)
		 * refreshes the admission centre, so the radius gate is never stale.
		 * Both feeders pass `authoritative` = true through this one setter and
		 * both run on the pump thread, so whichever happened last is the
		 * centre: a correction here overrides the walk, and the next sent
		 * position resumes tracking from wherever the correction put us.
		 * A same-dimension respawn clears no
		 * entries by design, so this is also the moment the columns around
		 * the DEATH point fall outside admit_radius + 1 and become
		 * demotable; the next pump's residency step re-evaluates against
		 * this centre and gives them back one per pump. No table clear, no
		 * lost state. */
		remote_admission_centre_set(remote, movement->x, movement->z, true);
	}
	if(movement->rotating) {
		remote->authoritative_yaw = movement->yaw;
		remote->authoritative_pitch = movement->pitch;
		remote->have_authoritative_look = true;
	}
	remote->authoritative_on_ground = movement->on_ground;
	remote->corrections_received++;

	/* Beta installs the server's position IMMEDIATELY on EVERY Packet10Flying
	 * -- including the login one that arrives long before terrain finishes
	 * streaming. setPositionAndRotation and the addToSendQueue echo both run
	 * unconditionally; only the doneLoadingTerrain flip is gated on "first
	 * packet" [740c583 NetClientHandler.handleFlying:242-267].
	 *
	 * This relay used to additionally require remote_load_complete_sent, which
	 * made take-51's pre-world echo dead code on the remote path: that echo
	 * (client_interface.c's mp_preworld_echo) arms on s_authoritative_pos_seen,
	 * and s_authoritative_pos_seen is set ONLY inside the CRPC_PLAYER_POS
	 * handler -- whose only two remote producers are this relay and
	 * remote_emit_load_completion_locked, i.e. the world had to be loaded
	 * before the client could send the position packets that make it load.
	 * Deadlock: the client sat at (0,0,0) sending nothing, mc173's
	 * update_chunks never ran again, and any spawn column missing from the
	 * server's initial unprompted burst never arrived, so
	 * remote_emit_load_completion_locked's spawn-chunk gate
	 * (server_remote_chunks.c) could never fire. Hardware signature:
	 * wl_pending=1 / world_loaded=false / chunks=0 / mp_ret_n pinned at 15 /
	 * pos=(0,0,0) for a full 480s with 4 MB received (mp_reconnect_soak
	 * 2026-09-01 x2, and mp_lan_walk_10m on the same evening) -- the same
	 * "N pre-chunks retained, then nothing" shape take-50 named.
	 *
	 * FINISH_REMOTE_LOAD is still withheld until that spawn-chunk gate fires,
	 * so CaveWii's sanctioned "park physics behind the load screen until the
	 * destination column installs" deviation (7ec5536) is untouched: only the
	 * position install, and the 50ms echo cadence it arms, become
	 * Beta-unconditional. */
	if(remote->have_authoritative_position) {
		/* correction_seq exists to invalidate client-authored motion samples
		 * that were already in flight when a correction was authored. Before
		 * the world install completes, the client has authored NOTHING (its
		 * only outbound position is the echo of this very packet), so a
		 * pre-world install has nothing to invalidate and must NOT consume a
		 * sequence number: the post-install correction stream stays
		 * byte-identical to what every already-passing fixture and every
		 * shipped build observed. Only real, post-load corrections bump it. */
		if(remote->remote_load_complete_sent
		   && ++remote->correction_seq == 0)
			remote->correction_seq = 1;
		uint8_t flags = PLAYER_CORRECTION_POSITION_ONLY
			| PLAYER_CORRECTION_ZERO_VEL_X | PLAYER_CORRECTION_ZERO_VEL_Y
			| PLAYER_CORRECTION_ZERO_VEL_Z | PLAYER_CORRECTION_SET_ON_GROUND;
		if(remote->authoritative_on_ground)
			flags |= PLAYER_CORRECTION_ON_GROUND;
		if(movement->rotating)
			flags |= PLAYER_CORRECTION_SET_ROTATION;
		struct client_rpc correction = {
			.type = CRPC_PLAYER_POS,
			.payload.player_pos = {
				.position = {(float)remote->authoritative_x,
					(float)remote->authoritative_y,
					(float)remote->authoritative_z},
				.rotation = {remote->authoritative_yaw,
					remote->authoritative_pitch},
				.correction_seq = remote->correction_seq,
				.correction_flags = flags,
			},
		};
		clin_process_direct_remote(&correction);
	}

	/* Beta echoes the same packet class, after rewriting moving coordinates to
	 * the installed body's x/minY/posY/z. [740c583 NetClientHandler.handleFlying] */
	struct mp_movement echo = *movement;
	if(echo.moving) {
		echo.x = remote->authoritative_x;
		echo.y = remote->authoritative_y - 1.62;
		echo.stance = remote->authoritative_y;
		echo.z = remote->authoritative_z;
	}
	(void)queue_movement(remote, &echo, event->packet.type, true);
}

/* mp-effects: the client's own cosmetic RNG for the aux-SFX rolls Beta draws
 * from the client's unsynced worldObj.rand / Math.random (never simulation). */
static struct random_gen s_aux_sfx_rand;
static bool s_aux_sfx_rand_seeded;
static float aux_sfx_rand(void) {
	if(!s_aux_sfx_rand_seeded) {
		rand_gen_seed(&s_aux_sfx_rand);
		s_aux_sfx_rand_seeded = true;
	}
	return rand_gen_flt(&s_aux_sfx_rand);
}

static void aux_sfx_sound(uint16_t sound_id, float x, float y, float z,
						  float vol, float pitch) {
	struct client_rpc call = {
		.type = CRPC_SOUND,
		.payload.sound = {
			.sound_id = sound_id,
			.pos = {x, y, z},
			.vol = vol,
			.pitch = pitch,
		},
	};
	clin_process_direct_remote(&call);
}

/* mp-effects: Packet61 -> World.playAuxSFX -> RenderGlobal.func_28136_a, the
 * whole Beta 1.7.3 effect table [740c583 RenderGlobal.java:1359-1418]. The
 * server sends these to every player in range EXCEPT the one who caused them
 * (ServerConfigurationManager.func_28171_a skips var1). Returns false for an
 * id Beta's client does not handle (counted by the caller). */
static bool remote_aux_sfx(int32_t effect, int32_t x, int32_t y, int32_t z,
						   int32_t data) {
	float fx = (float)x, fy = (float)y, fz = (float)z;
	switch(effect) {
		case 1000: /* dispenser fired: random.click 1.0, 1.0, RAW coords */
			aux_sfx_sound(SOUND_RANDOM_CLICK, fx, fy, fz, 1.0f, 1.0f);
			return true;
		case 1001: /* dispenser empty: random.click 1.0, 1.2 */
			aux_sfx_sound(SOUND_RANDOM_CLICK, fx, fy, fz, 1.0f, 1.2f);
			return true;
		case 1002: /* dispenser arrow: random.bow 1.0, 1.2 */
			aux_sfx_sound(SOUND_RANDOM_BOW, fx, fy, fz, 1.0f, 1.2f);
			return true;
		case 1003: {
			/* Door/trapdoor: Math.random() < 0.5 picks the FILE regardless
			 * of the real door state (data is always 0 from the server);
			 * pitch nextFloat()*0.1F+0.9F, block centre. */
			bool open = aux_sfx_rand() < 0.5f;
			aux_sfx_sound(open ? SOUND_RANDOM_DOOR_OPEN : SOUND_RANDOM_DOOR_CLOSE,
				fx + 0.5f, fy + 0.5f, fz + 0.5f, 1.0f,
				aux_sfx_rand() * 0.1f + 0.9f);
			return true;
		}
		case 1004: { /* fire extinguished: random.fizz 0.5, 2.6+(r-r)*0.8 */
			float a = aux_sfx_rand(), b = aux_sfx_rand();
			aux_sfx_sound(SOUND_RANDOM_FIZZ, fx + 0.5f, fy + 0.5f, fz + 0.5f,
				0.5f, 2.6f + (a - b) * 0.8f);
			return true;
		}
		case 1005: {
			/* Jukebox: playRecord(ItemRecord name or null). A non-record id
			 * (including 0, the eject) is Beta's stop. */
			struct client_rpc call = {
				.type = CRPC_RECORD_EVENT,
				.payload.record_event = {
					.pos = {fx, fy, fz},
					.record_id = (data == ITEM_RECORD_13
								  || data == ITEM_RECORD_CAT)
						? (uint16_t)data : 0,
				},
			};
			clin_process_direct_remote(&call);
			return true;
		}
		case 2000: {
			/* Dispenser smoke: ten directed particles. data = dx+1+(dz+1)*3;
			 * the existing CRPC_SMOKE_PUFF negative-intensity encoding
			 * carries it (server_local.c dispenser precedent), centre
			 * x+dx*0.6+0.5, y+0.5, z+dz*0.6+0.5. A negative data (never
			 * sent by Beta's BlockDispenser) has no encoding and is dropped. */
			if(data < 0)
				return false;
			int dx = data % 3 - 1;
			int dz = data / 3 % 3 - 1;
			struct client_rpc call = {
				.type = CRPC_SMOKE_PUFF,
				.payload.smoke_puff = {
					.pos = {fx + (float)dx * 0.6f + 0.5f, fy + 0.5f,
						fz + (float)dz * 0.6f + 0.5f},
					.intensity = (float)(-data - 1),
				},
			};
			clin_process_direct_remote(&call);
			return true;
		}
		case 2001: {
			/* Another player broke a block: the block's stepSoundDir at
			 * (vol+1)/2, pitch*0.8, centre, when id > 0. The particles
			 * (addBlockDestroyEffects) come from the Packet53 air-set that
			 * follows (CRPC_SET_BLOCK's dig burst reads the same block
			 * before clearing it), so they are not spawned twice here. */
			uint8_t id = (uint8_t)(data & 0xFF);
			if(id > 0) {
				uint16_t sid = audio_block_break_sound_id(id);
				if(sid != AUDIO_SOUND_NONE)
					aux_sfx_sound(sid, fx + 0.5f, fy + 0.5f, fz + 0.5f,
						(audio_block_sound_volume(id) + 1.0f) / 2.0f,
						audio_block_sound_pitch(id) * 0.8f);
			}
			return true;
		}
		default:
			return false;
	}
}

/* mp-effects: Beta's EntityLiving.handleHealthUpdate for OUR entity id
 * [740c583 NetClientHandler.func_9447_a -> getEntityByID returns thePlayer;
 * EntityLiving.java:850-861]: status 2 plays the hurt sound, status 3 the
 * death sound -- both "random.hurt" for a player, getSoundVolume() 1.0,
 * pitch (r-r)*0.2+1, at the player (playSoundAtEntity). The hurt flash and
 * health=0 come from the Packet8 the server sends with them. */
static void remote_own_status_sound(struct server_remote_impl* remote,
									int8_t status) {
	if(status != 2 && status != 3)
		return; /* Entity.handleHealthUpdate: no-op for a player */
	thread_mutex_lock(&remote->lock);
	float px = (float)remote->authoritative_x;
	float py = (float)(remote->authoritative_y - 1.62);
	float pz = (float)remote->authoritative_z;
	if(remote->have_last_sent_position) {
		px = (float)remote->last_sent_x;
		py = (float)remote->last_sent_y;
		pz = (float)remote->last_sent_z;
	}
	thread_mutex_unlock(&remote->lock);
	float a = aux_sfx_rand(), b = aux_sfx_rand();
	aux_sfx_sound(SOUND_RANDOM_HURT, px, py, pz, 1.0f, (a - b) * 0.2f + 1.0f);
}

/* mp-effects: Beta's PlayerControllerMP.syncCurrentPlayItem (:132-138)
 * compares inventory.currentItem with the last sent slot every tick. After a
 * login or respawn the server's player holds slot 0 (a fresh EntityPlayerMP;
 * currentItem is not saved), while this client keeps its selection, so the
 * selection is re-sent once there -- only when it differs from that 0, as
 * syncCurrentPlayItem's compare does (a slot-0 login stays byte-identical).
 * Without it the server places/uses the wrong item until the next scroll. */
static void remote_resync_held_slot(struct server_remote_impl* remote) {
	int slot = clin_selected_hotbar_slot();
	if(slot <= 0)
		return;
	struct mp_frame frame;
	if(mp_encode_block_item_switch(&frame, (int16_t)slot))
		(void)queue_frame_playing(remote, &frame);
}

bool process_event(struct server_remote* owner,
						  struct remote_event* event) {
	struct server_remote_impl* remote = owner->impl;
#ifdef PLATFORM_WII
	cw_dispatch_trace((int)event->kind, event->kind == REMOTE_EVENT_PACKET
											 ? (int)event->packet.type
											 : -1);
#endif
	if(event->kind == REMOTE_EVENT_CONNECTED) {
		thread_mutex_lock(&remote->lock);
		bool expected = remote->state == REMOTE_CONNECTING;
		thread_mutex_unlock(&remote->lock);
		if(!expected) {
			publish_error(remote, event->generation,
						  "connected event outside connecting state");
			return true;
		}
		struct mp_frame frame;
		if(!mp_encode_handshake(&frame, remote->username) || !queue_frame(remote, &frame)) {
			publish_error(remote, event->generation, "failed to queue handshake");
			return true;
		}
		thread_mutex_lock(&remote->lock);
		remote->state = REMOTE_HANDSHAKE_SENT;
		thread_mutex_unlock(&remote->lock);
		return true;
	}
	if(event->kind == REMOTE_EVENT_ERROR) {
		thread_mutex_lock(&remote->lock);
		/* GUARD 2 of 2 (mp-split-0912/5): a kick already settled WHY this
		 * session ended, so a transport error arriving afterwards is a
		 * CONSEQUENCE of the kick, not an explanation of it. The error
		 * side-slot is drained after the event queue, so on the normal
		 * "Packet255 then close" sequence this handler used to run last and
		 * unconditionally clobber the server's reason with "remote closed
		 * the socket" while demoting REMOTE_KICKED to REMOTE_CLOSING. Two
		 * independent signals veto that: the state already being
		 * REMOTE_KICKED, and reason_from_disconnect (which survives even if
		 * something else has since moved the state). The TEARDOWN half below
		 * still runs unconditionally -- dropping it too would risk the
		 * finding-5 wedge (an error that stops nothing leaves the session
		 * live forever) -- so only the reason/state overwrite is suppressed.
		 * Dropped errors are COUNTED, never silent
		 * (status.errors_dropped_after_kick, on the F3/heartbeat path). */
		bool kick_authoritative = remote->state == REMOTE_KICKED
			|| remote->reason_from_disconnect;
		if(kick_authoritative)
			remote->errors_dropped_after_kick++;
		else {
			set_reason_locked(remote, event->error);
			remote->state = REMOTE_CLOSING;
		}
		remote->stop_now = true;
		close_socket_locked(remote);
		thread_cond_broadcast(&remote->write_signal);
		thread_mutex_unlock(&remote->lock);
		return true;
	}

	/* hw1029b/G1: telemetry-only receive counters, taken before dispatch so a
	 * packet the handler below ignores still counts as received. */
	switch(event->packet.type) {
		case MP_EVENT_SLEEP: remote->kind_counts[SR_KIND_RX_BED]++; break;
		case MP_EVENT_ATTACH_ENTITY:
			remote->kind_counts[SR_KIND_RX_ATTACH]++;
			break;
		case MP_EVENT_NOTE_BLOCK:
			remote->kind_counts[SR_KIND_RX_BLOCK_ACTION]++;
			break;
		case MP_EVENT_DOOR_CHANGE:
			remote->kind_counts[SR_KIND_RX_AUX_SFX]++;
			break;
		case MP_EVENT_ENTITY_STATUS:
			if(event->packet.payload.entity_status.entity_id == remote->entity_id)
				remote->kind_counts[SR_KIND_RX_SELF_STATUS]++;
			break;
		case MP_EVENT_ENTITY_METADATA:
			remote->kind_counts[SR_KIND_RX_METADATA]++;
			break;
		default: break;
	}

	switch(event->packet.type) {
		case MP_EVENT_KEEP_ALIVE:
			thread_mutex_lock(&remote->lock);
			bool keep_alive_valid
				= remote->state == REMOTE_HANDSHAKE_SENT
				  || remote->state == REMOTE_LOGIN_SENT
				  || remote->state == REMOTE_PLAY;
			if(!keep_alive_valid) {
				thread_mutex_unlock(&remote->lock);
				publish_error(remote, event->generation,
							  "keep-alive outside connected protocol state");
				break;
			}
			remote->keep_alives_received++;
			thread_mutex_unlock(&remote->lock);
			(void)server_remote_send_keep_alive(owner);
			break;
		case MP_EVENT_HANDSHAKE:
			thread_mutex_lock(&remote->lock);
			bool handshake_valid = remote->state == REMOTE_HANDSHAKE_SENT;
			thread_mutex_unlock(&remote->lock);
			if(!handshake_valid) {
				publish_error(remote, event->generation,
							  "handshake reply outside handshake state");
				break;
			}
			/* Only "-" is offline mode; never contact the historical session URL.
			 * [740c583 NetClientHandler.handleHandshake] */
			if(!mp_utf16_equals_ascii(&event->packet.payload.handshake, "-")) {
				thread_mutex_lock(&remote->lock);
				set_reason_locked(remote, "online-mode authentication is unsupported");
				thread_mutex_unlock(&remote->lock);
				(void)server_remote_disconnect(owner, "Offline mode required");
				break;
			}
			// S5.3 hunt PROBE (temporary, ROUND 10 DISCRIMINATOR): was an
			// immediate mp_encode_login+queue_frame+state=LOGIN_SENT right
			// here -- now deferred ~3s (server_remote_pump polls and fires
			// it when due). state stays HANDSHAKE_SENT until the deferred
			// send actually happens; this event is a one-shot dequeue so
			// there's no re-entry risk. See server_remote_internal.h's field
			// comment for the full rationale.
			// ROUND 10 cycle 1: delay held at 0 -- this cycle isolates the
			// cw_cond_trace no-op (thread.c) alone, per the coordinator's
			// "3 normal non-delayed runs" ask. CW_LOGIN_DELAY_MS (defined
			// near this file's top) is the single knob for cycle 3's actual
			// discriminator once the SD-tracing confound is resolved.
			thread_mutex_lock(&remote->lock);
			remote->login_delay_pending = true;
			remote->login_delay_deadline
				= time_add_ms(time_get(), CW_LOGIN_DELAY_MS);
			thread_mutex_unlock(&remote->lock);
			break;
		case MP_EVENT_LOGIN:
			thread_mutex_lock(&remote->lock);
			if(remote->state != REMOTE_LOGIN_SENT) {
				thread_mutex_unlock(&remote->lock);
				publish_error(remote, event->generation,
							  "login reply outside login state");
				break;
			}
			if(event->packet.payload.login.dimension != WORLD_DIM_NETHER
			   && event->packet.payload.login.dimension != WORLD_DIM_OVERWORLD) {
				thread_mutex_unlock(&remote->lock);
				publish_error(remote, event->generation,
					"unsupported remote world dimension");
				break;
			}
			remote->logged_in = true;
			/* Response field 1 is the local entity ID; seed and dimension follow.
			 * [740c583 NetClientHandler.handleLogin]
			 * [740c583 Packet1Login.readPacketData] */
			remote->entity_id = event->packet.payload.login.entity_id;
			remote->map_seed = event->packet.payload.login.map_seed;
			remote->dimension = event->packet.payload.login.dimension;
			remote->applied_epoch = event->world_epoch;
			remote->mounted = false;
			remote->mounted_vehicle_id = -1;
			remote->local_sleeping = false; /* MP-BED: a fresh player */
			remote->state = REMOTE_PLAY;
			thread_mutex_unlock(&remote->lock);
			/* Fresh world: no S3 remote entities can legitimately exist yet
			 * (belt-and-suspenders -- server_remote_cancel already clears
			 * this on every teardown). */
			remote_entities_clear(remote);
			if(remote->climate)
				worldgen_destroy(remote->climate);
#ifdef PLATFORM_WII
			cw_dispatch_trace_bare("pre_worldgen_create");
			cw_stack_paint();
#endif
			ptime_t climate_start = time_get();
			remote->climate = worldgen_create(
				remote->map_seed, (enum world_dim)remote->dimension);
#ifdef PLATFORM_WII
			cw_dispatch_trace_bare(remote->climate ? "post_worldgen_create_ok"
													: "post_worldgen_create_null");
#endif
			int32_t climate_ms = time_diff_ms(climate_start, time_get());
			remote->climate_create_ms
				= climate_ms > 0 ? (uint32_t)climate_ms : 0;
			if(!remote->climate) {
				publish_error(remote, event->generation,
					"remote climate generator allocation failed");
				break;
			}
			/* Login constructs the remote WorldClient once; Packet6 is a separate
			 * spawn update and must not trigger another reset.
			 *
			 * S5.3 hunt fix (root cause hardware-proven takes 6-12,
			 * mp_connect_smoke): this used to route through
			 * remote_try_crpc()/clin_try_rpc_send() -- the clin_inbox
			 * tchannel every OTHER call site only ever produces into from
			 * the SERVER thread (see clin_try_rpc_send's own header
			 * comment). process_event runs on the MAIN/client thread here
			 * (server_remote_pump is called from main.c), so this was the
			 * first caller ever to collapse producer and consumer onto one
			 * thread through that path -- proven fatal: bracketing every
			 * primitive inside clin_try_rpc_send showed NONE of them ever
			 * ran; the run died in the call transition into
			 * remote_try_crpc/clin_try_rpc_send itself, right after this
			 * exact worldgen_create() returned cleanly. Fixed the same way
			 * S4's kick-path teardown fixed the identical wrong-thread-
			 * target mistake (e246a79): a first-class synchronous client-
			 * thread entry, clin_force_world_reset_login() (client_interface.c),
			 * that calls clin_process() directly -- same dispatch, zero
			 * tchannel/MQ indirection, two fewer stack frames. */
			clin_force_world_reset_login((enum world_dim)remote->dimension,
										 (uint32_t)remote->entity_id);
#ifdef PLATFORM_WII
			cw_dispatch_trace_bare("post_world_reset_crpc");
			cw_stack_scan("post_reset_watermark");
#endif
			(void)server_remote_send_keep_alive(owner);
#ifdef PLATFORM_WII
			cw_dispatch_trace_bare("post_keepalive");
#endif
			remote_resync_held_slot(remote); /* mp-effects: Packet16 */
			break;
		case MP_EVENT_UPDATE_TIME: {
			struct client_rpc call = {
				.type = CRPC_TIME_SET,
				.payload.time_set = (uint64_t)event->packet.payload.world_time,
			};
			clin_process_direct_remote(&call);
		} break;
		case MP_EVENT_SPAWN_POSITION:
			// S5.3 hunt fix, round 3 cont.: same wrong-thread-producer bug
			// as the LOGIN case just above (this codebase's process_event
			// runs on the main/client thread; remote_try_crpc/
			// clin_try_rpc_send's clin_inbox tchannel was designed for a
			// SERVER-thread producer only -- see clin_force_world_reset_login's
			// comment in client_interface.c for the full hardware-proven root
			// cause). Direct call, same fix shape.
			clin_force_world_spawn(event->packet.payload.spawn_position.x,
								   event->packet.payload.spawn_position.y,
								   event->packet.payload.spawn_position.z);
			/* F5b (review finding H5): the PROVISIONAL admission centre.
			 * Beta's login order is Packet1Login, Packet6SpawnPosition, the
			 * 0x0D teleport, THEN the chunk burst [740c583
			 * ServerConfigurationManager.login], so this is the earliest
			 * honest answer to "where is the player" -- and it is what keeps
			 * the radius gate from being INERT for a server that streams
			 * before it teleports (in which window every announced column
			 * used to install, 8 MiB worse than pre-slice because the
			 * backing reservation had already taken its cut of the carve).
			 * Passed as NON-authoritative: a real position packet outranks
			 * it and a later world-spawn can never overwrite one. */
			remote_admission_centre_set(remote,
				(double)event->packet.payload.spawn_position.x,
				(double)event->packet.payload.spawn_position.z, false);
			break;
		case MP_EVENT_UPDATE_HEALTH: {
			/* Packet8's healthMP is Beta's half-heart scale (full = 20:
			 * EntityPlayer ctor `this.health = 20`; the client applies the
			 * short raw: NetClientHandler.handleHealth -> setHealth(healthMP)
			 * [740c583 Packet8UpdateHealth]). CaveWii runs HEALTH_PER_HEART=16,
			 * so the old pass-through was 8x too small: a faithful server's
			 * full-health 20 rendered as 1.25 hearts. Scale-crossings convert
			 * at the wire boundary (armor-slice precedent). */
			struct client_rpc call = {
				.type = CRPC_PLAYER_SET_HEALTH,
				.payload.player_set_health.health
				= health_wire_to_internal(event->packet.payload.health),
			};
			clin_process_direct_remote(&call);
		} break;
		case MP_EVENT_FLYING:
		case MP_EVENT_PLAYER_POSITION:
		case MP_EVENT_PLAYER_LOOK:
		case MP_EVENT_PLAYER_LOOK_MOVE:
			remote_handle_correction(owner, event);
			break;
		case MP_EVENT_PRE_CHUNK:
		case MP_EVENT_MAP_CHUNK:
			/* Read-thread-owned bounded chunk path; never queued here. */
			break;
		case MP_EVENT_PLAYER_INVENTORY: {
			/* Packet5 targets a REMOTE entity's equipped slot -- S3 residual
			 * closed: CRPC_ENTITY_EQUIPMENT (not CRPC_INVENTORY_SLOT, which
			 * is local-player-only). [740c583 Packet5PlayerInventory] (plan
			 * section 6.3) */
			int32_t id = event->packet.payload.player_inventory.entity_id;
			if(id == remote->entity_id)
				break; /* the local player's own equipment has no P2 wire path */
			/* Packet5's EMPTY sentinel is NEGATIVE, not 0: Beta only builds a
			 * stack for itemID >= 0 (EntityOtherPlayerMP.outfitWithItem's
			 * `if(var2 >= 0)` [740c583]), and vanilla servers really do send
			 * -1 when a player empties their hand. The old unconditional u16
			 * cast turned -1 into 65535, which the CRPC's `item_id == 0`
			 * empty test then treated as a REAL item (adversarial-review
			 * CRITICAL: items[65535] indexed a 2304-entry registry in the
			 * render guard). Map every negative id to the internal empty
			 * sentinel 0 at this wire boundary. */
			int16_t wire_item = event->packet.payload.player_inventory.item_id;
			struct client_rpc call = {
				.type = CRPC_ENTITY_EQUIPMENT,
				.payload.entity_equipment = {
					.entity_id = (uint32_t)id,
					.slot = (uint8_t)event->packet.payload.player_inventory.slot,
					.item_id = wire_item < 0 ? 0 : (uint16_t)wire_item,
					.item_damage
					= event->packet.payload.player_inventory.item_damage,
				},
			};
			clin_process_direct_remote(&call);
		} break;
		case MP_EVENT_RESPAWN: {
			remote->active_window = 0;
			int8_t new_dimension = event->packet.payload.respawn_dimension;
			if(new_dimension != WORLD_DIM_NETHER
			   && new_dimension != WORLD_DIM_OVERWORLD) {
				publish_error(remote, event->generation,
					"unsupported remote respawn dimension");
				break;
			}
			thread_mutex_lock(&remote->lock);
			bool dimension_changed = new_dimension != remote->dimension;
			if(dimension_changed) {
				remote->dimension = new_dimension;
				/* The old world's chunk entries were dropped by the READ thread
				 * at this Respawn's position in the stream (transport.c); by now
				 * the table may already hold the NEW world's bodies, which must
				 * survive. Applying the epoch releases them for promotion. */
				remote->applied_epoch = event->world_epoch;
				remote_entities_clear(remote);
				remote->have_authoritative_position = false;
				remote->have_authoritative_look = false;
				/* F5b: the centre belonged to the world whose entries the
				 * read thread just dropped. Clearing it means the new
				 * world's first columns are bounded by the hard resident cap
				 * until its SpawnPosition/PositionLook arrives, which is the
				 * honest state -- a stale centre would refuse everything
				 * (H5's mirror case) and leave all 441 columns BACKED. */
				remote_admission_centre_clear(remote);
				remote->remote_load_complete_sent = false;
				remote->load_completion_pending = false;
			}
			thread_mutex_unlock(&remote->lock);
			/* MP-BED: Minecraft.respawn builds a fresh EntityPlayer (sleeping
			 * false, sleepTimer 0) [NetClientHandler.java:467-478]; a bed
			 * state the server never woke must not survive it. */
			if(remote->local_sleeping) {
				remote->local_sleeping = false;
				struct client_rpc fresh = {
					.type = CRPC_SLEEP_STATE,
					.payload.sleep_state = {.timer = 0, .sleeping = false},
				};
				clin_process_direct_remote(&fresh);
			}
			/* MP-RIDE: the fresh player rides nothing either, and Beta's
			 * server sends no Packet39(-1) for a dead rider
			 * (ServerConfigurationManager.recreatePlayerEntity builds a new
			 * EntityPlayerMP). Keeping `mounted` would keep the -999 riding
			 * movement form (EntityClientPlayerMP.java:63-70) after respawn.
			 * The vehicle link is released WITHOUT Beta's dismount
			 * reposition -- there is no dismount, the old body is gone. */
			if(remote->mounted) {
				struct client_rpc release = {
					.type = CRPC_ATTACH_ENTITY,
					.payload.attach_entity = {
						.vehicle_id = (uint32_t)remote->mounted_vehicle_id,
						.attach = false,
						.no_reposition = true,
					},
				};
				remote->mounted = false;
				remote->mounted_vehicle_id = -1;
				clin_process_direct_remote(&release);
			}
			if(!dimension_changed) {
				/* Fix (2026-08-30, respawn-hunt investigation residual):
				 * Beta's respawn always installs a FRESH, full-health player
				 * [740c583 EntityPlayer ctor: health = 20;
				 * NetClientHandler.handleRespawn ->
				 * ServerConfigurationManager.respawnPlayer], matching this
				 * file's own dimension-changed branch below (its
				 * CRPC_WORLD_RESET constructs a brand-new local_player and
				 * sets health = MAX_PLAYER_HEALTH as part of that). The
				 * same-dimension branch does NOT rebuild the entity (no
				 * world swap), so it must reset health explicitly -- without
				 * this, a live server that (correctly, per protocol) sends
				 * no separate Packet8 alongside Respawn left the client
				 * reading its OWN stale health <= 0 forever: verified via
				 * --live-respawn-hunt-fall against mc173-diag, whose
				 * handle_respawn only sends Respawn+SpawnPosition+
				 * PositionLook, never Packet8 (mc173-diag's own citation is
				 * the same NetClientHandler::handleRespawn reference this
				 * comment cites -- the server relies on the client already
				 * knowing respawn means full health, exactly like Beta's own
				 * client does). Beta's full health is 20 WIRE half-hearts and
				 * this CRPC carries INTERNAL units (HEALTH_PER_HEART=16, max
				 * 160), so the constant goes through the same
				 * health_wire_to_internal() adapter as MP_EVENT_UPDATE_HEALTH
				 * -- the first landing passed a raw 20 (2.5 hearts) because it
				 * predated the b976144 Packet8 scale-seam fix and matched the
				 * then-unscaled convention. */
				struct client_rpc respawn_health = {
					.type = CRPC_PLAYER_SET_HEALTH,
					.payload.player_set_health.health
					= health_wire_to_internal(20),
				};
				clin_process_direct_remote(&respawn_health);
				remote_resync_held_slot(remote); /* mp-effects: Packet16 */
				break; /* Same-dimension respawn: no world swap [740c583
						* NetClientHandler.handleRespawn]. */
			}
			if(remote->climate)
				worldgen_destroy(remote->climate);
			remote->climate = worldgen_create(
				remote->map_seed, (enum world_dim)new_dimension);
			if(!remote->climate) {
				publish_error(remote, event->generation,
					"remote respawn climate generator allocation failed");
				break;
			}
			/* Split-dims B1: a dimension change is a new remote world -- a
			 * fresh session identity, bound by this reset and carried by
			 * every remote-origin RPC after it. */
			struct client_rpc reset = {
				.type = CRPC_WORLD_RESET,
				.payload.world_reset = {
					.dimension = (enum world_dim)new_dimension,
					.local_entity = (uint32_t)remote->entity_id,
					.world_index = 0,
					.world_generation = clin_remote_world_generation_mint(),
				},
			};
			clin_process_direct_remote(&reset);
			remote_resync_held_slot(remote); /* mp-effects: Packet16 */
		} break;
		case MP_EVENT_MULTI_BLOCK_CHANGE: {
			const struct mp_multi_block_change* change
				= &event->packet.payload.multi_block_change;
			int32_t base_x = change->chunk_x * 16;
			int32_t base_z = change->chunk_z * 16;
			for(uint16_t i = 0; i < change->count; i++) {
				uint16_t coord = change->coordinate[i];
				/* [740c583 NetClientHandler.handleMultiBlockChange] */
				int32_t local_x = (coord >> 12) & 15;
				int32_t local_z = (coord >> 8) & 15;
				uint8_t local_y = (uint8_t)(coord & 255);
				remote_apply_block_change(remote, base_x + local_x, local_y,
					base_z + local_z, change->type[i], change->metadata[i]);
				/* B2 seam: one echo per CELL, in the packet's own cell
				 * order (recording only -- the apply above is unchanged). */
				remote_record_block_echo(remote, base_x + local_x, local_y,
					base_z + local_z, change->type[i], change->metadata[i]);
			}
			/* This event's decoder-owned arrays are never queued further;
			 * release them here (mirrors CRPC_CHUNK's slot ownership
			 * ending at clin consumption -- this boundary just has no
			 * pooled slot to hand off). */
			mp_event_destroy(&event->packet);
		} break;
		case MP_EVENT_BLOCK_CHANGE:
			remote_apply_block_change(remote,
				event->packet.payload.block_change.x,
				event->packet.payload.block_change.y,
				event->packet.payload.block_change.z,
				event->packet.payload.block_change.block_type,
				event->packet.payload.block_change.metadata);
			(void)remote_sign_place_echo(remote,
				event->packet.payload.block_change.x,
				event->packet.payload.block_change.y,
				event->packet.payload.block_change.z,
				event->packet.payload.block_change.block_type);
			/* B2 seam: the dig/place receipt (recording only). */
			remote_record_block_echo(remote,
				event->packet.payload.block_change.x,
				event->packet.payload.block_change.y,
				event->packet.payload.block_change.z,
				event->packet.payload.block_change.block_type,
				event->packet.payload.block_change.metadata);
			break;
		case MP_EVENT_OPEN_WINDOW: {
			if(event->packet.payload.open_window.window == WINDOWC_INVENTORY) {
				static bool logged_reserved_window;
				if(!logged_reserved_window) {
					logged_reserved_window = true;
					fprintf(stderr, "[mp] ignoring OpenWindow for reserved inventory window 0\n");
				}
				break;
			}
			uint8_t inventory_type
				= event->packet.payload.open_window.inventory_type;
			/* Beta's Packet100 inventoryType numerically matches CaveWii's
			 * own enum window_type for the 4 wire-registered kinds (0
			 * chest, 1 workbench, 2 furnace, 3 dispenser); anything else
			 * has no client screen. [740c583 Packet100OpenWindow] [740c583
			 * NetClientHandler.handleOpenWindow] */
			if(inventory_type > WINDOW_TYPE_DISPENSER)
				break;
			/* Packet100 counts container storage only. The vanilla client adds
			 * its 36 player slots (ContainerChest.java:14-27); workbench and
			 * furnace constructors have fixed layouts. */
			size_t capacity = inventory_type == WINDOW_TYPE_WORKBENCH ? CRAFTING_SIZE
				: inventory_type == WINDOW_TYPE_FURNACE ? FURNACE_SIZE
				: (size_t)event->packet.payload.open_window.slot_count + 36;
			if(capacity > CRPC_WINDOW_ITEMS_MAX_SLOTS) break;
			remote->active_window = (uint8_t)event->packet.payload.open_window.window;
			thread_mutex_lock(&remote->lock);
			remote->progress_window = event->packet.payload.open_window.window;
			remote->progress_cook = remote->progress_burn
				= remote->progress_fuel = 0;
			thread_mutex_unlock(&remote->lock);
			struct client_rpc call = {
				.type = CRPC_OPEN_WINDOW,
				.payload.window_open = {
					.window
					= (uint8_t)event->packet.payload.open_window.window,
					.type = (enum window_type)inventory_type,
					.slot_count = (uint8_t)capacity,
				},
			};
			clin_process_direct_remote(&call);
		} break;
		case MP_EVENT_CLOSE_WINDOW: {
			remote->active_window = 0;
			struct client_rpc call = {
				.type = CRPC_WINDOW_CLOSE_REMOTE,
				.payload.window_close_remote.window
				= (uint8_t)event->packet.payload.close_window,
			};
			clin_process_direct_remote(&call);
		} break;
		case MP_EVENT_SET_SLOT: {
			int8_t window = event->packet.payload.set_slot.window;
			int16_t slot = event->packet.payload.set_slot.slot;
			if(window == -1 && slot == -1) {
				window = (int8_t)remote->active_window;
				slot = SPECIAL_SLOT_PICKED_ITEM;
			} else if(window < 0 || slot < 0 || slot >= SPECIAL_SLOT_PICKED_ITEM) break;

			struct client_rpc call = {
				.type = CRPC_INVENTORY_SLOT,
				.payload.inventory_slot = {
					.window = (uint8_t)window,
					.slot = (uint8_t)slot,
					.item = item_data_from_mp(
						&event->packet.payload.set_slot.item),
				},
			};
			clin_process_direct_remote(&call);
			/* B2 seam (recording only), AFTER the dispatch and with the
			 * RESOLVED window/slot -- the cursor form (-1, -1) above became
			 * (active window, SPECIAL_SLOT_PICKED_ITEM), and the two `break`
			 * paths this never reaches are set-slots the handler already
			 * drops, which changed nothing and must not be credited. */
			remote_record_set_slot(remote, window, slot,
				&event->packet.payload.set_slot.item);
		} break;
		case MP_EVENT_WINDOW_ITEMS: {
			int8_t window = event->packet.payload.window_items.window;
			uint16_t count = event->packet.payload.window_items.count;
			if(window < 0) {
				/* No cursor-window batch case exists on the wire. */
				mp_event_destroy(&event->packet);
				break;
			}
			if(count > CRPC_WINDOW_ITEMS_MAX_SLOTS) {
				/* Honest cap, not a silent truncation: no real Beta
				 * container plus its appended player inventory reaches
				 * this size. Force a clean reconnect exactly like every
				 * other hostile-length codec rejection. */
				thread_mutex_lock(&remote->lock);
				remote->forced_reconnects++;
				set_reason_locked(remote,
					"remote window item count exceeds client cap");
				remote->stop_now = true;
				close_socket_locked(remote);
				thread_cond_broadcast(&remote->write_signal);
				thread_mutex_unlock(&remote->lock);
				mp_event_destroy(&event->packet);
				break;
			}
			struct client_rpc call = {.type = CRPC_WINDOW_ITEMS};
			call.payload.window_items.window = (uint8_t)window;
			call.payload.window_items.count = count;
			for(uint16_t i = 0; i < count; i++)
				call.payload.window_items.items[i] = item_data_from_mp(
					&event->packet.payload.window_items.items[i]);
			clin_process_direct_remote(&call);
			/* B2 seam (recording only): a whole-window refresh is ONE record
			 * on the same serial, marked slot -1 / item -1 -- the batch's
			 * per-slot states are the client window's record, not this
			 * ring's. The two `break` paths above (no cursor-window batch on
			 * the wire, and the hostile-length rejection) delivered nothing,
			 * so they are not credited. */
			remote_record_set_slot(remote, window, -1, NULL);
			/* The CRPC embeds a copied array (not a pointer hand-off), so
			 * this event's decoder-owned array is done after the copy
			 * above regardless of clin_process_direct's dispatch. */
			mp_event_destroy(&event->packet);
		} break;
		case MP_EVENT_UPDATE_PROGRESSBAR: {
			int8_t window = event->packet.payload.update_progressbar.window;
			int16_t bar = event->packet.payload.update_progressbar.bar;
			int16_t value = event->packet.payload.update_progressbar.value;
			if(window < 0 || (bar != 0 && bar != 1 && bar != 2))
				break; /* Retained-but-unhandled bar id: no non-furnace
						* consumer exists yet [740c583
						* Packet105UpdateProgressbar]. */
			thread_mutex_lock(&remote->lock);
			if(remote->progress_window != window) {
				remote->progress_window = window;
				remote->progress_cook = remote->progress_burn
					= remote->progress_fuel = 0;
			}
			if(bar == 0)
				remote->progress_cook = (uint16_t)value;
			else if(bar == 1)
				remote->progress_burn = (uint16_t)value;
			else
				remote->progress_fuel = (uint16_t)value;
			uint16_t cook = remote->progress_cook;
			uint16_t burn = remote->progress_burn;
			uint16_t fuel = remote->progress_fuel;
			thread_mutex_unlock(&remote->lock);
			/* CRPC_WINDOW_PROGRESS overwrites all three fields together
			 * (client_interface.c), so resend the full tracked triple
			 * every time -- Packet105 itself only ever updates one. */
			struct client_rpc call = {
				.type = CRPC_WINDOW_PROGRESS,
				.payload.window_progress = {
					.window = (uint8_t)window,
					.cook_time = cook,
					.burn_time = burn,
					.current_item_burn_time = fuel,
				},
			};
			clin_process_direct_remote(&call);
		} break;
		case MP_EVENT_TRANSACTION: {
			int8_t window = event->packet.payload.transaction.window;
			bool accepted = event->packet.payload.transaction.accepted;
			struct client_rpc call = {
				.type = CRPC_WINDOW_TRANSACTION,
				.payload.window_transaction = {
					.window = (uint8_t)window,
					.action_id
					= (uint16_t)event->packet.payload.transaction.action,
					.accepted = accepted,
				},
			};
			clin_process_direct_remote(&call);
			/* B2 seam (recording only): the RECEIVED result, both verdicts.
			 * The outbound ack this client queues for a rejected click is
			 * SRPC_WINDOW_TRANSACTION and is deliberately NOT recorded here
			 * -- it is our word, not the server's. */
			remote_record_txn_result(remote, window,
				event->packet.payload.transaction.action, accepted);
		} break;
		case MP_EVENT_UPDATE_SIGN: {
			/* Inbound sign text (another player's live edit, or a sign
			 * whose text arrives separately from its chunk). Forwards to
			 * the client-side sign_text_store (source/game/sign_text_store.h)
			 * via the same CRPC_SIGN_TEXT the single-player chunk-send and
			 * edit-echo paths use -- this closes the documented S3 residual
			 * (plan 6.3's UpdateSign note previously covered only the
			 * outbound direction). [740c583 Packet130UpdateSign]
			 *
			 * Beta's line is a variable-length String (readString(...,15),
			 * up to MP_SIGN_LINE_MAX UTF-16 code units); sign_text_store
			 * instead keys a fixed 4x16 right-space-padded grid with no NULs
			 * (the te_sign convention). Each line is copied unit-for-unit
			 * into its row and the remaining columns stay space-padded,
			 * mirroring remote_flush_sign_edit's own OUTBOUND trim in
			 * reverse. A code unit outside this engine's single-byte grid
			 * (never emitted by this project's own char-cycle OSK, but a
			 * real Beta server's String is unconstrained) and an embedded
			 * NUL both substitute ' ' rather than corrupt the grid or
			 * violate its no-NULs contract. */
			_Static_assert(MP_SIGN_LINE_COUNT == SIGN_TEXT_LINES,
						  "Packet130's 4-line loop must match the "
						  "sign_text_store grid's line count");
			char grid[SIGN_TEXT_LINES * SIGN_TEXT_COLS];
			memset(grid, ' ', sizeof(grid));
			for(size_t line = 0; line < MP_SIGN_LINE_COUNT; line++) {
				const struct mp_utf16* src
					= &event->packet.payload.update_sign.lines[line];
				size_t n = src->length;
				if(n > SIGN_TEXT_COLS - 1) /* column 15 stays reserved-blank,
				                            * matching TE_SIGN_LINE_MAX */
					n = SIGN_TEXT_COLS - 1;
				for(size_t col = 0; col < n; col++) {
					uint16_t unit = src->units[col];
					grid[line * SIGN_TEXT_COLS + col]
						= (unit == 0 || unit > 0xFFu) ? ' ' : (char)unit;
				}
			}
			struct client_rpc call = {
				.type = CRPC_SIGN_TEXT,
				.payload.sign_text = {
					.x = event->packet.payload.update_sign.x,
					.y = event->packet.payload.update_sign.y,
					.z = event->packet.payload.update_sign.z,
				},
			};
			memcpy(call.payload.sign_text.grid, grid, sizeof(grid));
			clin_process_direct_remote(&call);
		} break;

		/* --- S3: entities and remote players ------------------------------
		 * Every case below that carries an `entity_id` first checks it
		 * against remote->entity_id (the LOCAL player's id from login) and
		 * no-ops if it matches: a real Beta server never targets P2
		 * spawn/move/destroy traffic at the client's own id (Packet10-13 own
		 * player-position correction exclusively), so this is a defensive
		 * guard, not a translation path -- it exists purely to guarantee the
		 * remote-entity table can never shadow or "spawn a ghost" of the
		 * local player. [740c583 NetClientHandler.getEntityByID] */
		case MP_EVENT_SLEEP: {
			/* MP-BED: the packet's entity id is looked up; when that
			 * entity exists and the packet's state field is 0, ...
			 * the player sleeps in the bed at (x, y, z) (NetClientHandler.java:370-378).
			 * The server sends it to the sleeper ITSELF too
			 * (EntityPlayerMP.java:239-243, right after the teleportTo that
			 * already put us in the bed), so our own id is the bed state, not
			 * a ghost: the local player enters the sleep state at timer 0
			 * (EntityPlayer.java:583-584), which blocks movement
			 * (isMovementBlocked, EntityPlayer.java:127-129) and opens the
			 * Leave Bed screen (Minecraft.java:963-964). */
			const struct mp_event* pkt = &event->packet;
			if(pkt->payload.sleep.unknown != 0)
				break;
			int32_t id = pkt->payload.sleep.entity_id;
			if(id == remote->entity_id) {
				remote->local_sleeping = true;
				struct client_rpc call = {
					.type = CRPC_SLEEP_STATE,
					.payload.sleep_state = {.timer = 0, .sleeping = true},
				};
				clin_process_direct_remote(&call);
				break;
			}
			struct remote_entity* entry = remote_entity_find(remote, id);
			if(!entry || entry->kind != REMOTE_ENTITY_PLAYER)
				break; /* getEntityByID null, or not an EntityPlayer */
			struct client_rpc call = {
				.type = CRPC_ENTITY_ANIMATION,
				.payload.entity_animation = {
					.entity_id = (uint32_t)id,
					.use_bed = true,
					.bed_x = pkt->payload.sleep.bed_x,
					.bed_y = (uint8_t)pkt->payload.sleep.bed_y,
					.bed_z = pkt->payload.sleep.bed_z,
				},
			};
			clin_process_direct_remote(&call);
		} break;
		case MP_EVENT_ANIMATION: {
			int32_t id = event->packet.payload.animation.entity_id;
			if(id == remote->entity_id) {
				/* MP-BED: our own animate 3 is the server's wakeUpPlayer
				 * (EntityPlayerMP.java:249-252 sends it to the sleeper too);
				 * Beta's client runs wakeUpPlayer(false, false, false)
				 * (NetClientHandler.java:359-361): sleeping = false and
				 * sleepTimer = 100, so the fade runs out to 110
				 * (EntityPlayer.java:628-636). The server's teleportTo that
				 * follows places us beside the bed. Other own-id codes stay
				 * ignored (swing/hurt are local-authored here). */
				if(event->packet.payload.animation.animate == 3) {
					remote->local_sleeping = false;
					struct client_rpc call = {
						.type = CRPC_SLEEP_STATE,
						.payload.sleep_state = {.timer = 100, .sleeping = false},
					};
					clin_process_direct_remote(&call);
				}
				break;
			}
			struct client_rpc call = {
				.type = CRPC_ENTITY_ANIMATION,
				.payload.entity_animation = {
					.entity_id = (uint32_t)id,
					.animate = event->packet.payload.animation.animate,
				},
			};
			clin_process_direct_remote(&call);
		} break;
		case MP_EVENT_NAMED_ENTITY_SPAWN: {
			const struct mp_event* pkt = &event->packet;
			int32_t id = pkt->payload.named_entity_spawn.entity_id;
			if(id == remote->entity_id)
				break;
			struct remote_entity* entry
				= remote_entity_create(remote, id, REMOTE_ENTITY_PLAYER);
			if(!entry)
				break; /* table full: counted in capacity_drops */
			entry->server_x = pkt->payload.named_entity_spawn.x;
			entry->server_y = pkt->payload.named_entity_spawn.y;
			entry->server_z = pkt->payload.named_entity_spawn.z;
			entry->yaw = mp_byte_angle_to_degrees(
				pkt->payload.named_entity_spawn.rotation);
			entry->pitch
				= mp_byte_angle_to_degrees(pkt->payload.named_entity_spawn.pitch);
			struct client_rpc call = {.type = CRPC_SPAWN_PLAYER};
			call.payload.spawn_player.entity_id = (uint32_t)id;
			if(!mp_utf16_to_utf8(&pkt->payload.named_entity_spawn.name,
								 call.payload.spawn_player.username,
								 sizeof(call.payload.spawn_player.username)))
				call.payload.spawn_player.username[0] = '\0';
			call.payload.spawn_player.pos[0]
				= (float)mp_fixed32_to_double(entry->server_x);
			call.payload.spawn_player.pos[1]
				= (float)mp_fixed32_to_double(entry->server_y);
			call.payload.spawn_player.pos[2]
				= (float)mp_fixed32_to_double(entry->server_z);
			/* Wire (Beta) frame -> ENGINE frame at the CRPC boundary. The
			 * tracker entry keeps raw Beta wire degrees; every CRPC
			 * orientation payload carries engine-frame degrees: yaw NEGATED
			 * (Beta forward is (-sin, cos) [740c583 Entity.moveFlying],
			 * engine yaw is atan2f(dx, dz) -> engine == -beta; the local
			 * player's SEND path already applies this same mirror,
			 * client_interface.c SRPC_PLAYER_POS `rx = -glm_deg(camera.rx)`)
			 * and pitch +90 (inverse of that path's `ry = glm_deg(...) -
			 * 90`). Without the negation every remote look was REFLECTED --
			 * a peer facing east drew facing west (adversarial-review HIGH). */
			call.payload.spawn_player.yaw = -entry->yaw;
			call.payload.spawn_player.pitch = entry->pitch + 90.0F;
			/* currentItem == 0 is Beta's own "empty" sentinel (count 1 /
			 * damage 0 implied when nonzero). [740c583
			 * NetClientHandler.handleNamedEntitySpawn] A NEGATIVE id is not a
			 * legal Packet20 value, but a hostile/buggy server can send one;
			 * clamp to empty rather than let the u16 cast forge id 65535+
			 * (same class as the Packet5 CRITICAL above). */
			call.payload.spawn_player.held_item_id
				= pkt->payload.named_entity_spawn.current_item < 0
				? 0
				: (uint16_t)pkt->payload.named_entity_spawn.current_item;
			clin_process_direct_remote(&call);
		} break;
		case MP_EVENT_PICKUP_SPAWN: {
			const struct mp_event* pkt = &event->packet;
			int32_t id = pkt->payload.pickup_spawn.entity_id;
			if(id == remote->entity_id)
				break;
			struct remote_entity* entry
				= remote_entity_create(remote, id, REMOTE_ENTITY_ITEM);
			if(!entry)
				break;
			entry->server_x = pkt->payload.pickup_spawn.x;
			entry->server_y = pkt->payload.pickup_spawn.y;
			entry->server_z = pkt->payload.pickup_spawn.z;
			/* Packet21PickupSpawn.readPacketData [740c583]:
			 * `this.itemID = var1.readShort();` -- a plain signed
			 * readShort() with no non-negative validation, structurally
			 * identical to Packet5PlayerInventory's and Packet20
			 * NamedEntitySpawn's own item-id fields (both clamped above in
			 * this same file). A hostile/buggy server sending a negative
			 * itemID must not forge id 65535+ through the unconditional u16
			 * cast (same class as the Packet5 CRITICAL above) -- clamp to
			 * the internal empty sentinel 0. */
			int16_t wire_item_id = pkt->payload.pickup_spawn.item_id;
			struct client_rpc call = {
				.type = CRPC_SPAWN_ITEM,
				.payload.spawn_item = {
					.entity_id = (uint32_t)id,
					.item = {.id = wire_item_id < 0 ? 0 : (uint16_t)wire_item_id,
						.durability
						= (uint16_t)pkt->payload.pickup_spawn.item_damage,
						.count = pkt->payload.pickup_spawn.count},
					.pos = {(float)mp_fixed32_to_double(entry->server_x),
						(float)mp_fixed32_to_double(entry->server_y),
						(float)mp_fixed32_to_double(entry->server_z)},
					/* rotation/pitch/roll are velocity /128.0, NOT angles.
					 * [740c583 NetClientHandler.handlePickupSpawn] */
					.vel = {(float)pkt->payload.pickup_spawn.vel_x / 128.0f,
						(float)pkt->payload.pickup_spawn.vel_y / 128.0f,
						(float)pkt->payload.pickup_spawn.vel_z / 128.0f},
				},
			};
			clin_process_direct_remote(&call);
		} break;
		/* collector_entity_id stays EXEMPT from the "check against
		 * remote->entity_id" invariant stated in the block comment above: it
		 * LEGITIMATELY equals remote->entity_id on every ordinary "you picked
		 * up your own drop" event -- exactly the case CRPC_PICKUP_ITEM's
		 * consumer exists to handle (client_interface.c gates its
		 * pickup-flourish on `collector_id == gstate.players[0].local_player->id`); a
		 * blanket guard would suppress the local player's own pickup
		 * animation. collected_entity_id DOES take the guard: this case now
		 * removes that entity (below), so a hostile collected==self must be
		 * dropped like every other destroy-capable case.
		 * [740c583 Packet22Collect] */
		case MP_EVENT_COLLECT: {
			int32_t collected = event->packet.payload.collect.collected_entity_id;
			struct client_rpc call = {
				.type = CRPC_PICKUP_ITEM,
				.payload.pickup_item = {
					.entity_id = (uint32_t)collected,
					.collector_id = (uint32_t)
						event->packet.payload.collect.collector_entity_id,
				},
			};
			clin_process_direct_remote(&call);
			/* Beta handleCollect plays "random.pop" 0.2F, pitch
			 * ((r-r)*0.7F+1.0F)*2.0F AT THE COLLECTED ENTITY, for any
			 * collector and any entity kind (no type check, so arrows pop
			 * too), only when the entity is known client-side. The pitch is
			 * rolled per collect with Beta's exact formula from the client's
			 * cosmetic RNG (Beta uses its own unsynced Random; never a sim
			 * stream). [Beta 1.7.3 NetClientHandler.handleCollect:
			 * playSoundAtEntity] */
			struct remote_entity* popped = remote_entity_find(remote, collected);
			if(popped && collected != remote->entity_id) {
				struct client_rpc pop = {
					.type = CRPC_SOUND,
					.payload.sound = {
						.sound_id = SOUND_RANDOM_POP,
						.pos = {(float)mp_fixed32_to_double(popped->server_x),
							(float)mp_fixed32_to_double(popped->server_y),
							(float)mp_fixed32_to_double(popped->server_z)},
						.vol = 0.2f,
						.pitch = 0.0f,
					},
				};
				struct random_gen* pop_rng = audio_cosmetic_rng();
				float pop_r1 = rand_gen_flt(pop_rng);
				float pop_r2 = rand_gen_flt(pop_rng);
				pop.payload.sound.pitch = ((pop_r1 - pop_r2) * 0.7F + 1.0F) * 2.0F;
				clin_process_direct_remote(&pop);
			}
			/* Beta's client REMOVES the collected entity in the packet
			 * handler itself -- whoever the collector is -- and the server
			 * never owes a follow-up destroy for it (mc173 sends none; see
			 * docs/fork_and_prior_art_sweep_2026-08-26.md par.E tracker notes).
			 * Without this, another player's pickup left a permanent
			 * client-side ghost item (maintainer live repro 2026-08-27).
			 * Same removal pair as MP_EVENT_DESTROY_ENTITY; both consumers
			 * absorb an unknown id as a no-op. [740c583
			 * NetClientHandler.handleCollect: removeEntityFromWorld] */
			if(collected != remote->entity_id) {
				remote_entity_destroy(remote, collected);
				struct client_rpc destroy = {
					.type = CRPC_ENTITY_DESTROY,
					.payload.entity_destroy.entity_id = (uint32_t)collected,
				};
				clin_process_direct_remote(&destroy);
			}
		} break;
		case MP_EVENT_VEHICLE_SPAWN: {
			const struct mp_event* pkt = &event->packet;
			int32_t id = pkt->payload.vehicle_spawn.entity_id;
			if(id == remote->entity_id)
				break;
			uint8_t vtype = pkt->payload.vehicle_spawn.vtype;
			vec3 pos = {(float)mp_fixed32_to_double(pkt->payload.vehicle_spawn.x),
				(float)mp_fixed32_to_double(pkt->payload.vehicle_spawn.y),
				(float)mp_fixed32_to_double(pkt->payload.vehicle_spawn.z)};
			vec3 vel = {0};
			fprintf(stderr, "[mp] vehicle spawn id=%d type=%u at %.2f %.2f %.2f\n",
					(int)id, (unsigned)vtype, pos[0], pos[1], pos[2]);
			if(pkt->payload.vehicle_spawn.has_velocity) {
				/* [740c583 NetClientHandler.handleVehicleSpawn] */
				vel[0] = (float)pkt->payload.vehicle_spawn.speed_x / 8000.0f;
				vel[1] = (float)pkt->payload.vehicle_spawn.speed_y / 8000.0f;
				vel[2] = (float)pkt->payload.vehicle_spawn.speed_z / 8000.0f;
			}
			/* Types [740c583 EntityList.<clinit> / NetClientHandler.
			 * handleVehicleSpawn]: 10/11/12 minecart (rideable/chest/
			 * furnace), 1 boat, 50 primed TNT, 60/61/62/63 arrow/snowball/
			 * egg/fireball (this engine's shared CRPC_SPAWN_ARROW billboard,
			 * matching server_local's own egg/snowball/fireball mapping),
			 * 90 the fishing hook (card m2: the same billboard server_local
			 * uses for its own bobber), 70/71 falling sand/gravel (card m3:
			 * the SP CRPC_SPAWN_FALLING_SAND render). Any other type has no
			 * client render/CRPC in this engine -- tracked UNSUPPORTED so a
			 * later DESTROY is absorbed, not errored. */
			if(vtype == 10 || vtype == 11 || vtype == 12) {
				struct remote_entity* entry
					= remote_entity_create(remote, id, REMOTE_ENTITY_MINECART);
				if(!entry)
					break;
				struct client_rpc call = {.type = CRPC_SPAWN_MINECART};
				call.payload.spawn_minecart.entity_id = (uint32_t)id;
				glm_vec3_copy(pos, call.payload.spawn_minecart.pos);
				glm_vec3_copy(vel, call.payload.spawn_minecart.vel);
				call.payload.spawn_minecart.minecart_type = vtype - 10;
				call.payload.spawn_minecart.shake_dir = 1;
				clin_process_direct_remote(&call);
			} else if(vtype == 1) {
				struct remote_entity* entry
					= remote_entity_create(remote, id, REMOTE_ENTITY_BOAT);
				if(!entry)
					break;
				struct client_rpc call = {.type = CRPC_SPAWN_BOAT};
				call.payload.spawn_boat.entity_id = (uint32_t)id;
				glm_vec3_copy(pos, call.payload.spawn_boat.pos);
				glm_vec3_copy(vel, call.payload.spawn_boat.vel);
				clin_process_direct_remote(&call);
			} else if(vtype == 50) {
				struct remote_entity* entry = remote_entity_create(
					remote, id, REMOTE_ENTITY_PRIMED_TNT);
				if(!entry)
					break;
				struct client_rpc call = {
					.type = CRPC_SPAWN_PRIMED_TNT,
					.payload.spawn_primed_tnt = {
						.entity_id = (uint32_t)id,
						.fuse = 80, /* EntityTNTPrimed's own constructor default;
									 * Beta's wire spawn carries no fuse field. */
					},
				};
				glm_vec3_copy(pos, call.payload.spawn_primed_tnt.pos);
				glm_vec3_copy(vel, call.payload.spawn_primed_tnt.vel);
				clin_process_direct_remote(&call);
			} else if(vtype == 60 || vtype == 61 || vtype == 62 || vtype == 63) {
				struct remote_entity* entry = remote_entity_create(
					remote, id, REMOTE_ENTITY_PROJECTILE);
				if(!entry)
					break;
				uint16_t item_id = vtype == 60 ? ITEM_ARROW
					: vtype == 62 ? ITEM_EGG
									: ITEM_SNOW_BALL; /* 61 snowball, 63 fireball
														* (server_local's own
														* shared-sprite choice) */
				struct client_rpc call = {
					.type = CRPC_SPAWN_ARROW,
					.payload.spawn_arrow = {.entity_id = (uint32_t)id,
						.item_id = item_id, .fireball = vtype == 63},
				};
				glm_vec3_copy(pos, call.payload.spawn_arrow.pos);
				glm_vec3_copy(vel, call.payload.spawn_arrow.vel);
				clin_process_direct_remote(&call);
				/* MP client-side shot sound, played from the observed spawn
				 * (the spawn position IS the shooter: Beta launches every
				 * projectile at the shooter's body). Formulas are the fire
				 * sites' own, at this file's fixed-midpoint-pitch convention
				 * (no cosmetic RNG stream here -- the note-block/explosion
				 * precedent above): arrow = "random.bow" 1.0F, pitch
				 * 1.0F/(r*0.4F+0.8F) -> 1.0F [740c583 ItemBow.
				 * onPlayerStoppedUsing; EntitySkeleton.attackEntity is
				 * identical]; snowball/egg = "random.bow" 0.5F,
				 * 0.4F/(r*0.4F+0.8F) -> 0.4F [ItemSnowball/ItemEgg.
				 * onItemRightClick]; fireball = "mob.ghast.fireball" at the
				 * ghast's getSoundVolume() 10F, (r-r)*0.2F+1.0F -> 1.0F
				 * [EntityGhast.onLivingUpdate:85 -- the sound accompanies the
				 * EntityFireball spawn, so the observed spawn is its faithful
				 * moment]. DOCUMENTED ADAPTATION for 60-62: Beta MP plays the
				 * bow/throw twang only on the SHOOTING client (item code runs
				 * client-side; b1.7.3 has no sound wire) -- observers heard
				 * nothing. The observed-spawn trigger restores the audible
				 * event for everyone, the maintainer-ordered quality call
				 * (task_09eb9063); it also covers our OWN shots, which echo
				 * back as this same spawn (RTT-delayed). */
				struct client_rpc shot = {
					.type = CRPC_SOUND,
					.payload.sound = {
						.sound_id = vtype == 63 ? SOUND_MOB_GHAST_FIREBALL
												: SOUND_RANDOM_BOW,
						.pos = {pos[0], pos[1], pos[2]},
						.vol = vtype == 60 ? 1.0f
							: vtype == 63 ? 10.0f
											: 0.5f,
						.pitch = vtype == 61 || vtype == 62 ? 0.4f : 1.0f,
					},
				};
				clin_process_direct_remote(&shot);
			} else if(vtype == 90) {
				/* Card m2 (docs/intent/m2-mp-remote-bobber.md): Beta builds an
				 * EntityFish(worldClient, x, y, z) for type 90
				 * [NetClientHandler.java:80-82] -- the position-only
				 * constructor, so the client hook has NO angler: the thrower
				 * field only gates the setVelocity [NetClientHandler.
				 * java:125-133] (its owner lookup is type 60 only), and
				 * RenderFish draws the line only `if(angler != null)`
				 * [RenderFish.java:34] -- a remote hook shows the float and
				 * no line, for every viewer including the angler himself.
				 * Rendered with the SAME client entity server_local sends for
				 * its own bobber (CRPC_SPAWN_ARROW carrying ITEM_FISHING_ROD,
				 * server_local.c server_local_fishing_rod_cast), which also
				 * draws no line; moves/velocity/destroy then ride the
				 * projectile kind like 60-63. No sound: the cast's
				 * "random.bow" is played by ItemFishingRod on the casting
				 * client only [ItemFishingRod.java:24] (the observed-spawn
				 * twang adaptation above is scoped to 60-62). */
				struct remote_entity* entry = remote_entity_create(
					remote, id, REMOTE_ENTITY_PROJECTILE);
				if(!entry)
					break;
				struct client_rpc call = {
					.type = CRPC_SPAWN_ARROW,
					.payload.spawn_arrow = {.entity_id = (uint32_t)id,
						.item_id = ITEM_FISHING_ROD},
				};
				glm_vec3_copy(pos, call.payload.spawn_arrow.pos);
				glm_vec3_copy(vel, call.payload.spawn_arrow.vel);
				clin_process_direct_remote(&call);
			} else if(vtype == 70 || vtype == 71) {
				/* Card m3 (docs/intent/m3-mp-remote-falling-sand.md): Beta
				 * builds EntityFallingSand(worldClient, x, y, z,
				 * Block.sand.blockID) for 70 and Block.gravel.blockID for 71
				 * [NetClientHandler.java:109-116]. The client copy is the
				 * one server_local already streams in single player
				 * (CRPC_SPAWN_FALLING_SAND -> entity_falling_sand.c): it
				 * falls under its own Beta physics between packets, is
				 * snapped by Packet31/33/34 (remote_base_entity_snap names
				 * ENTITY_FALLING_SAND), takes Packet28, and waits on the
				 * ground for the server's Packet29 -- the landed block
				 * arrives as Packet53, like Beta's server-side placement.
				 * Beta's client-side origin clear / landing set
				 * [EntityFallingSand.onUpdate] are WorldClient predictions
				 * the server's own Packet53s overwrite; not repeated. */
				struct remote_entity* entry = remote_entity_create(
					remote, id, REMOTE_ENTITY_FALLING_SAND);
				if(!entry)
					break;
				struct client_rpc call = {
					.type = CRPC_SPAWN_FALLING_SAND,
					.payload.spawn_falling_sand = {
						.entity_id = (uint32_t)id,
						.block_id = vtype == 70 ? BLOCK_SAND : BLOCK_GRAVEL,
					},
				};
				glm_vec3_copy(pos, call.payload.spawn_falling_sand.pos);
				glm_vec3_copy(vel, call.payload.spawn_falling_sand.vel);
				clin_process_direct_remote(&call);
			} else {
				(void)remote_entity_create(remote, id, REMOTE_ENTITY_UNSUPPORTED);
			}
			/* Beta seeds serverPosX/Y/Z from the spawn for EVERY Packet23
			 * kind [NetClientHandler.java:118-120], so the first Packet31/33
			 * relative move adds its byte deltas to the spawn point.
			 * remote_entity_create zeroes the entry, and no vehicle branch
			 * above wrote these (the Packet20/21/24 spawns do): a vehicle's
			 * first relative move landed it at (delta/32) near the world
			 * origin. Seeded once here for every kind the chain created
			 * (card m2 found it: the hook moves by Packet31). A failed create
			 * `break`s out of the case above and never reaches this. */
			struct remote_entity* seeded = remote_entity_find(remote, id);
			if(seeded) {
				seeded->server_x = pkt->payload.vehicle_spawn.x;
				seeded->server_y = pkt->payload.vehicle_spawn.y;
				seeded->server_z = pkt->payload.vehicle_spawn.z;
			}
		} break;
		case MP_EVENT_MOB_SPAWN: {
			const struct mp_event* pkt = &event->packet;
			int32_t id = pkt->payload.mob_spawn.entity_id;
			if(id == remote->entity_id)
				break;
			int monster_id;
			if(!beta_mob_type_to_monster_id(pkt->payload.mob_spawn.mtype,
											&monster_id)) {
				(void)remote_entity_create(remote, id, REMOTE_ENTITY_UNSUPPORTED);
				break; /* Giant/generic Mob/Monster: no engine equivalent yet. */
			}
			struct remote_entity* entry
				= remote_entity_create(remote, id, REMOTE_ENTITY_MOB);
			if(!entry)
				break;
			entry->monster_id = monster_id;
			entry->server_x = pkt->payload.mob_spawn.x;
			entry->server_y = pkt->payload.mob_spawn.y;
			entry->server_z = pkt->payload.mob_spawn.z;
			entry->yaw = mp_byte_angle_to_degrees(pkt->payload.mob_spawn.yaw);
			entry->pitch = mp_byte_angle_to_degrees(pkt->payload.mob_spawn.pitch);
			apply_mob_metadata(entry, &pkt->payload.mob_spawn.metadata);
			struct client_rpc call = {
				.type = CRPC_SPAWN_MONSTER,
				.payload.spawn_monster = {
					.entity_id = (uint32_t)id,
					.monster_id = entry->monster_id,
					.fleece_color = entry->fleece_color,
					.sheared = entry->sheared,
					.saddled = entry->saddled,
					.wolf_flags = entry->wolf_flags,
					.wolf_health = entry->wolf_health,
					.charged = entry->charged,
					.pos = {(float)mp_fixed32_to_double(entry->server_x),
						(float)mp_fixed32_to_double(entry->server_y),
						(float)mp_fixed32_to_double(entry->server_z)},
				},
			};
			clin_process_direct_remote(&call);
			/* Packet24 carries the full DataWatcher at track start. Reuse
			 * Packet40's flag decoder after the mob exists on the client. */
			const struct mp_watchable_entry* e0 = metadata_find(
				&pkt->payload.mob_spawn.metadata, 0, 0);
			if(e0) {
				struct mp_entity_metadata_flags flags
					= mp_decode_entity_metadata_flags(e0->value.byte_value);
				struct client_rpc flag_call = {
					.type = CRPC_ENTITY_FLAGS,
					.payload.entity_flags = {
						.entity_id = (uint32_t)id,
						.sneaking = flags.sneaking,
						.on_fire = flags.on_fire,
						.is_riding = flags.is_riding,
					},
				};
				clin_process_direct_remote(&flag_call);
			}
		} break;
		case MP_EVENT_ENTITY_PAINTING: {
			/* [740c583 Packet25EntityPainting / NetClientHandler.
			 * handleEntityPainting]: entityId, title, x/y/zPosition (the
			 * WALL block -- this engine's tile_x/y/z), direction. Beta's
			 * client-side handler constructs `new EntityPainting(world, x,
			 * y, z, direction, title)`, matching this engine's
			 * painting_art_by_title(title) -> entity_painting(...,
			 * server=false) pair one-for-one -- see client_interface.c's
			 * CRPC_SPAWN_PAINTING case, which this reuses rather than
			 * duplicating. */
			const struct mp_event* pkt = &event->packet;
			int32_t id = pkt->payload.entity_painting.entity_id;
			if(id == remote->entity_id)
				break;
			struct remote_entity* entry
				= remote_entity_create(remote, id, REMOTE_ENTITY_PAINTING);
			if(!entry)
				break;
			/* DELIBERATE ADAPTATION from Beta: an unmatched title leaves
			 * Beta's own `this.art` field null (EntityPainting(World,x,y,z,
			 * dir,title)'s match loop has no Kebab fallback, unlike
			 * readEntityFromNBT's), which crashes RenderPainting's very
			 * next frame (var10.sizeX on a null EnumArt reference) -- not a
			 * behaviour worth replicating for a malformed/foreign server's
			 * title string. painting_art_by_title's existing Kebab(0)
			 * fallback (already used for the NBT-restore path) is reused
			 * here for crash-safety; every LEGITIMATE Beta title still
			 * resolves to its exact table entry. */
			char title_ascii[PAINTING_ART_TITLE_MAX];
			if(!mp_utf16_to_utf8(&pkt->payload.entity_painting.title,
								 title_ascii, sizeof(title_ascii)))
				title_ascii[0] = '\0'; // painting_art_by_title(NULL/no-match) = Kebab
			int art_index = painting_art_by_title(title_ascii);
			struct client_rpc call = {
				.type = CRPC_SPAWN_PAINTING,
				.payload.spawn_painting = {
					.entity_id = (uint32_t)id,
					.art_index = art_index,
					.direction = pkt->payload.entity_painting.direction,
					.tile_x = pkt->payload.entity_painting.x,
					.tile_y = pkt->payload.entity_painting.y,
					.tile_z = pkt->payload.entity_painting.z,
				},
			};
			clin_process_direct_remote(&call);
		} break;
		case MP_EVENT_ENTITY_VELOCITY: {
			/* Residual item 1 (2026-08-26): [740c583 NetClientHandler.
			 * handleEntityVelocity -> Entity.setVelocity(motion/8000)].
			 * Beta applies this to every entity class via the identical base
			 * setVelocity, but the EFFECT differs by class -- verified
			 * against the decompile, correcting a common assumption that
			 * "mobs/players both skip it":
			 *   - EntityOtherPlayerMP overrides onLivingUpdate and never
			 *     calls moveEntityWithHeading, so a remote OTHER PLAYER's
			 *     velocity has NO movement effect at all (only feeds a
			 *     cosmetic arm/leg-bob amplitude via cameraYaw/cameraPitch,
			 *     which this engine's entity_remote_player.c does not model
			 *     either). [740c583 EntityOtherPlayerMP.onLivingUpdate]
			 *   - EntityLiving (mobs) is NOT overridden this way: it calls
			 *     this.moveEntityWithHeading(moveStrafing, moveForward)
			 *     UNCONDITIONALLY every tick, and moveEntityWithHeading calls
			 *     this.moveEntity(motionX, motionY, motionZ) in every branch
			 *     -- so a remote MOB's velocity DOES drive real per-tick
			 *     physics in Beta. [740c583 EntityLiving.onLivingUpdate:642,
			 *     .moveEntityWithHeading:454-459]
			 * Despite that last finding, THIS engine's client_tick_monster
			 * (entity_monster.c) has NO e->vel consumer at all -- a remote
			 * mob is 100% network-position-approach driven, never physics-
			 * integrated client-side (matching server_local's own SP mobs,
			 * whose tick_server already moved e->pos before tick_client
			 * runs). Forwarding velocity for REMOTE_ENTITY_MOB would
			 * therefore be a dead store in THIS engine regardless of Beta's
			 * own behaviour -- an architecture gap, not a parity gap. Skip
			 * both MOB and PLAYER; forward for every kind whose client tick
			 * already integrates e->vel every frame (the item/vehicle/
			 * projectile precedent the base-entity-snap fix established --
			 * see client_interface.c's CRPC_ENTITY_VELOCITY case for the
			 * exact type list). */
			const struct mp_event* pkt = &event->packet;
			int32_t id = pkt->payload.entity_velocity.entity_id;
			if(id == remote->entity_id) {
				/* mp-velocity-0926 (astra-knockback-0926 item 3): the OWN
				 * entity id is the client's EntityPlayerSP, which Beta's
				 * handleEntityVelocity (NetClientHandler.java:165-170) looks
				 * up like any other entity and setVelocity()s -- all three
				 * components, raw/8000 (Entity.setVelocity :1000-1004). The
				 * server sends it after knockBack
				 * (EntityTrackerEntry.updatePlayerList:124-127). Route it to
				 * the host body through the same client apply path the local
				 * server's knockback uses; a remote session installs X/Y/Z
				 * (clin_apply_player_velocity). */
				struct client_rpc call = {
					.type = CRPC_LOCAL_PLAYER_VELOCITY,
					.payload.local_player_velocity = {
						.owner = 0,
						.vel = {
							(float)pkt->payload.entity_velocity.vel_x / 8000.0f,
							(float)pkt->payload.entity_velocity.vel_y / 8000.0f,
							(float)pkt->payload.entity_velocity.vel_z / 8000.0f,
						},
					},
				};
				/* The stamped remote dispatch, like every other remote
				 * site (G6 S1 follow-up): CRPC_LOCAL_PLAYER_VELOCITY is not
				 * world-scoped today, so routing is unchanged; the envelope
				 * now carries the session identity for consistency. */
				clin_process_direct_remote(&call);
				break;
			}
			struct remote_entity* entry = remote_entity_find(remote, id);
			if(!entry)
				break;
			switch(entry->kind) {
				case REMOTE_ENTITY_ITEM:
				case REMOTE_ENTITY_MINECART:
				case REMOTE_ENTITY_BOAT:
				case REMOTE_ENTITY_PROJECTILE:
				case REMOTE_ENTITY_PRIMED_TNT:
				case REMOTE_ENTITY_FALLING_SAND: {
					struct client_rpc call = {
						.type = CRPC_ENTITY_VELOCITY,
						.payload.entity_velocity = {
							.entity_id = (uint32_t)id,
							.vel = {
								(float)pkt->payload.entity_velocity.vel_x
									/ 8000.0f,
								(float)pkt->payload.entity_velocity.vel_y
									/ 8000.0f,
								(float)pkt->payload.entity_velocity.vel_z
									/ 8000.0f,
							},
						},
					};
					clin_process_direct_remote(&call);
				} break;
				default: break; /* MOB/PLAYER/UNSUPPORTED: see the comment above. */
			}
		} break;
		case MP_EVENT_DESTROY_ENTITY: {
			int32_t id = event->packet.payload.destroy_entity.entity_id;
			if(id == remote->entity_id)
				break;
			if(remote->mounted && remote->mounted_vehicle_id == id) {
				/* The vehicle died under us: Beta's updateRidden drops a dead
				 * ridingEntity on the next tick. The client entity goes away
				 * with the destroy below, which ends the tether. */
				remote->mounted = false;
				remote->mounted_vehicle_id = -1;
			}
			remote_entity_destroy(remote, id);
			struct client_rpc call = {
				.type = CRPC_ENTITY_DESTROY,
				.payload.entity_destroy.entity_id = (uint32_t)id,
			};
			clin_process_direct_remote(&call);
		} break;
		case MP_EVENT_ENTITY:
			/* Heartbeat only -- no position/look data on the wire. [740c583
			 * Packet30Entity.readPacketData] */
			break;
		case MP_EVENT_REL_ENTITY_MOVE:
		case MP_EVENT_REL_ENTITY_MOVE_LOOK:
		case MP_EVENT_ENTITY_TELEPORT: {
			int32_t id, dx = 0, dy = 0, dz = 0;
			int8_t yaw_byte = 0, pitch_byte = 0;
			bool has_look = false, absolute = false;
			int32_t abs_x = 0, abs_y = 0, abs_z = 0;
			if(event->packet.type == MP_EVENT_REL_ENTITY_MOVE) {
				id = event->packet.payload.rel_entity_move.entity_id;
				dx = event->packet.payload.rel_entity_move.dx;
				dy = event->packet.payload.rel_entity_move.dy;
				dz = event->packet.payload.rel_entity_move.dz;
			} else if(event->packet.type == MP_EVENT_REL_ENTITY_MOVE_LOOK) {
				id = event->packet.payload.rel_entity_move_look.entity_id;
				dx = event->packet.payload.rel_entity_move_look.dx;
				dy = event->packet.payload.rel_entity_move_look.dy;
				dz = event->packet.payload.rel_entity_move_look.dz;
				yaw_byte = event->packet.payload.rel_entity_move_look.yaw;
				pitch_byte = event->packet.payload.rel_entity_move_look.pitch;
				has_look = true;
			} else {
				id = event->packet.payload.entity_teleport.entity_id;
				abs_x = event->packet.payload.entity_teleport.x;
				abs_y = event->packet.payload.entity_teleport.y;
				abs_z = event->packet.payload.entity_teleport.z;
				yaw_byte = event->packet.payload.entity_teleport.yaw;
				pitch_byte = event->packet.payload.entity_teleport.pitch;
				has_look = absolute = true;
			}
			if(id == remote->entity_id)
				break;
			struct remote_entity* entry = remote_entity_find(remote, id);
			if(!entry)
				break; /* spawn never seen/dropped -- Beta's own getEntityByID
						 * null guard has the identical effect. */
			if(absolute) {
				entry->server_x = abs_x;
				entry->server_y = abs_y;
				entry->server_z = abs_z;
			} else {
				/* Beta's OWN accumulation: plain int addition, not a
				 * fixed-point-then-double round trip, so repeated relative
				 * moves stay byte-exact. [740c583
				 * NetClientHandler.handleEntity] */
				entry->server_x += dx;
				entry->server_y += dy;
				entry->server_z += dz;
			}
			if(has_look) {
				entry->yaw = mp_byte_angle_to_degrees(yaw_byte);
				entry->pitch = mp_byte_angle_to_degrees(pitch_byte);
			}
			struct client_rpc call = {.type = CRPC_ENTITY_MOVE};
			call.payload.entity_move.entity_id = (uint32_t)id;
			/* Every Beta move/teleport arms a 3-increment client approach --
			 * the client then lands exactly on this target in <=3 ticks
			 * instead of snapping (players) or decaying toward it forever
			 * (mobs). The METADATA-carrier MOVE below deliberately does NOT
			 * arm (a metadata packet moves nothing). [740c583
			 * NetClientHandler.handleEntity] [740c583
			 * NetClientHandler.handleEntityTeleport] (both pass increments 3).
			 */
			call.payload.entity_move.approach_ticks = 3;
			call.payload.entity_move.pos[0]
				= (float)mp_fixed32_to_double(entry->server_x);
			call.payload.entity_move.pos[1]
				= (float)mp_fixed32_to_double(entry->server_y)
				/* Teleports only: Beta lifts the client target by 1/64 block
				 * (serverPosY/32.0 + 0.015625D); relative moves do not. The
				 * bias stays OUT of entry->server_y so the int accumulation
				 * above remains byte-exact. [740c583
				 * NetClientHandler.handleEntityTeleport] */
				+ (absolute ? 0.015625F : 0.0F);
			call.payload.entity_move.pos[2]
				= (float)mp_fixed32_to_double(entry->server_z);
			/* Same carry rule for the Packet38 hurt flash: the handler
			 * writes e->hurt unconditionally, so an in-window move must
			 * not clear it. */
			call.payload.entity_move.hurt = entry->hurt_active;
			if(entry->kind == REMOTE_ENTITY_MOB) {
				/* Carry the cached cosmetic flags along -- client_interface.c's
				 * CRPC_ENTITY_MOVE handler overwrites them unconditionally, so
				 * omitting them would reset sheared/saddled/wolf/charged to
				 * false on every move. */
				call.payload.entity_move.death_time = entry->death_time_sent;
				call.payload.entity_move.fleece_color = entry->fleece_color;
				call.payload.entity_move.sheared = entry->sheared;
				call.payload.entity_move.saddled = entry->saddled;
				call.payload.entity_move.wolf_flags = entry->wolf_flags;
				call.payload.entity_move.wolf_health = entry->wolf_health;
				call.payload.entity_move.charged = entry->charged;
				call.payload.entity_move.ghast_attacking
					= entry->ghast_attacking;
			}
			clin_process_direct_remote(&call);
			/* Render follow-up: Packet33/34's look half (has_look) forwarded
			 * the SAME way Packet32 does below -- REMOTE_ENTITY_PLAYER and
			 * REMOTE_ENTITY_MOB both have a client-side consumer as of
			 * 2026-08-26 residual item 2 (client_interface.c's
			 * CRPC_ENTITY_LOOK case gates on ENTITY_REMOTE_PLAYER/
			 * ENTITY_MONSTER; a mob's head yaw approaches this wire target via
			 * its OWN dedicated pair -- entity.h's network_head_yaw comment --
			 * falling back to movement-derived once consumed, Beta's
			 * non-rotating case), so this is still scoped by kind rather than
			 * sent unconditionally like CRPC_ENTITY_MOVE above (no other kind
			 * has any orientation consumer). [740c583 Packet33RelEntityMoveLook]
			 * [740c583 Packet34EntityTeleport] */
			if(has_look
			   && (entry->kind == REMOTE_ENTITY_PLAYER
				   || entry->kind == REMOTE_ENTITY_MOB)) {
				struct client_rpc look = {
					.type = CRPC_ENTITY_LOOK,
					.payload.entity_look = {
						.entity_id = (uint32_t)id,
						/* Beta wire -> engine frame: see the CRPC_SPAWN_PLAYER
						 * boundary note (yaw mirrored, pitch +90). */
						.yaw = -entry->yaw,
						.pitch = entry->pitch + 90.0F,
						/* Rotation steps through the same 3 Beta increments
						 * as position. [740c583 NetClientHandler.handleEntity] */
						.approach_ticks = 3,
					},
				};
				clin_process_direct_remote(&look);
			}
		} break;
		case MP_EVENT_ENTITY_LOOK: {
			int32_t id = event->packet.payload.entity_look.entity_id;
			if(id == remote->entity_id)
				break;
			struct remote_entity* entry = remote_entity_find(remote, id);
			if(!entry)
				break;
			entry->yaw
				= mp_byte_angle_to_degrees(event->packet.payload.entity_look.yaw);
			entry->pitch = mp_byte_angle_to_degrees(
				event->packet.payload.entity_look.pitch);
			/* Render follow-up (2026-08-19; extended to mobs 2026-08-26
			 * residual item 2): forward it the same way the
			 * REL_ENTITY_MOVE_LOOK/ENTITY_TELEPORT branch above does, scoped
			 * to REMOTE_ENTITY_PLAYER and REMOTE_ENTITY_MOB -- both now have a
			 * client-side consumer for a networked head yaw (see that
			 * branch's own comment). [740c583 Packet32EntityLook] */
			if(entry->kind == REMOTE_ENTITY_PLAYER
			   || entry->kind == REMOTE_ENTITY_MOB) {
				struct client_rpc look = {
					.type = CRPC_ENTITY_LOOK,
					.payload.entity_look = {
						.entity_id = (uint32_t)id,
						/* Beta wire -> engine frame: see the CRPC_SPAWN_PLAYER
						 * boundary note (yaw mirrored, pitch +90). */
						.yaw = -entry->yaw,
						.pitch = entry->pitch + 90.0F,
						/* Rotation steps through the same 3 Beta increments
						 * as position. [740c583 NetClientHandler.handleEntity] */
						.approach_ticks = 3,
					},
				};
				clin_process_direct_remote(&look);
			}
		} break;
		case MP_EVENT_ENTITY_STATUS: {
			/* [740c583 NetClientHandler.handleEntityStatus ->
			 * EntityLiving.handleHealthUpdate]: status 2 = hurt flash
			 * (hurtTime = maxHurtTime = 10 ticks) + the hurt-flail rising
			 * edge the client monster tick already consumes from the MOVE
			 * mirror; status 3 = death (health = 0 -> the client-run death
			 * roll -- driven here by flush_entity_status's wall clock,
			 * since MP servers send the status once and the SP
			 * progression stream does not exist). Other codes fall to
			 * Entity.handleHealthUpdate, a no-op in Beta -- same here.
			 * RESOLVED 2026-08-26 (residual item 4): the hurt/death SOUND
			 * Beta plays inline (getHurtSound/getDeathSound at status 2/3,
			 * same handleHealthUpdate lines) now routes through
			 * remote_emit_status_sound -- see that function for the mob vs
			 * player sound-source split. */
			int32_t id = event->packet.payload.entity_status.entity_id;
			int8_t status = event->packet.payload.entity_status.status;
			if(id == remote->entity_id) {
				/* mp-effects: Beta sends 2/3 to the victim too
				 * (WorldServer.java:86-88), and that is where our own
				 * hurt/death sound comes from. */
				remote_own_status_sound(remote, status);
				break;
			}
			struct remote_entity* entry = remote_entity_find(remote, id);
			if(!entry
			   || (entry->kind != REMOTE_ENTITY_MOB
				   && entry->kind != REMOTE_ENTITY_PLAYER))
				break;
			if(status == 2) {
				entry->hurt_active = true;
				entry->hurt_until = time_add_ms(time_get(), 500);
				remote_emit_status_move(remote, entry);
				remote_emit_status_sound(remote, entry, status);
			} else if(status == 3) {
				/* Sound is one-shot per corpse regardless of kind (a
				 * dedicated latch -- see death_sound_played's own comment).
				 * Animation-parity pass (2026-09-03) item 2: the visual
				 * death-ROLL was MOB-only before this pass ("stays MOB-only",
				 * the old comment here) -- but RenderLiving.rotateCorpse's
				 * deathTime flop [740c583] applies to EVERY EntityLiving,
				 * players included (EntityPlayer IS an EntityLiving; only a
				 * dying remote player previously had no client render
				 * consumer for the SAME networked deathTime a mob already
				 * gets). Widened to REMOTE_ENTITY_PLAYER too, still gated on
				 * death_active so a second status==3 for the same corpse is a
				 * no-op. */
				if(!entry->death_sound_played) {
					entry->death_sound_played = true;
					remote_emit_status_sound(remote, entry, status);
				}
				if((entry->kind == REMOTE_ENTITY_MOB
					|| entry->kind == REMOTE_ENTITY_PLAYER)
				   && !entry->death_active) {
					entry->death_active = true;
					entry->death_started = time_get();
					entry->death_time_sent = 1; /* Beta's first dying tick */
					remote_emit_status_move(remote, entry);
				}
			}
		} break;
		case MP_EVENT_ATTACH_ENTITY: {
			/* Mount/dismount, the Beta client way for ANY entity pair:
			 * the rider is looked up by the packet's entity id and the
			 * vehicle by its vehicle id; when the rider id is the local
			 * player's id, the rider is the local player; a found rider
			 * then runs mountEntity on the vehicle (NetClientHandler.java:
			 * 439-449). A vanilla server sends Packet39 only down the mounting
			 * player's own connection (EntityPlayerMP.java:261-265 is its only
			 * sender), so a remote rider normally shows through the Packet40
			 * riding flag (CRPC_ENTITY_FLAGS below); a server that does name
			 * another player is honoured the same way (MP-RIDE). Design and
			 * Beta citations: docs/mp_mounts_design_2026-09-11.md. */
			int32_t rider = event->packet.payload.attach_entity.entity_id;
			int32_t vehicle = event->packet.payload.attach_entity.vehicle_entity_id;
			fprintf(stderr, "[mp] attach rider=%d vehicle=%d (us %d, mounted %d)\n",
					(int)rider, (int)vehicle, (int)remote->entity_id,
					remote->mounted ? 1 : 0);
			if(rider != remote->entity_id) {
				/* MP-RIDE: another player. getEntityByID(rider) must be a
				 * tracked player (only EntityOtherPlayerMP has a rider model
				 * here); an unknown vehicle id is Beta's mountEntity(null). */
				struct remote_entity* rider_entry = remote_entity_find(remote, rider);
				if(!rider_entry || rider_entry->kind != REMOTE_ENTITY_PLAYER)
					break;
				struct remote_entity* vehicle_entry
					= vehicle < 0 ? NULL : remote_entity_find(remote, vehicle);
				struct client_rpc call = {
					.type = CRPC_ATTACH_ENTITY,
					.payload.attach_entity = {
						.vehicle_id = vehicle_entry ? (uint32_t)vehicle : 0,
						.attach = vehicle_entry != NULL,
						.remote_rider = true,
						.rider_id = (uint32_t)rider,
					},
				};
				clin_process_direct_remote(&call);
				break;
			}
			if(vehicle < 0) {
				/* Dismount: release whatever we were riding. A detach while
				 * not riding is Beta's mountEntity(null) with ridingEntity
				 * null -- a no-op. */
				if(!remote->mounted)
					break;
				struct client_rpc call = {
					.type = CRPC_ATTACH_ENTITY,
					.payload.attach_entity = {
						.vehicle_id = (uint32_t)remote->mounted_vehicle_id,
						.attach = false,
					},
				};
				remote->mounted = false;
				remote->mounted_vehicle_id = -1;
				remote->attach_applied++;
				clin_process_direct_remote(&call);
				break;
			}
			struct remote_entity* entry = remote_entity_find(remote, vehicle);
			if(!entry || entry->kind == REMOTE_ENTITY_PLAYER) {
				/* "Fails visibly": a vehicle we never spawned or already
				 * destroyed is counted, never applied. (Beta would run
				 * mountEntity(null) for it; keeping the current mount is the
				 * pre-existing, test-pinned choice -- the server's own -1
				 * detach still releases it.) A player is never a vehicle in
				 * vanilla Beta. MP-RIDE: every other tracked kind -- boat,
				 * minecart, and the saddled pig (EntityPig.interact ->
				 * mountEntity, EntityPig.java) -- is ridden. */
				remote->attach_ignored++;
				break;
			}
			if(remote->mounted && remote->mounted_vehicle_id != vehicle) {
				/* Beta's mountEntity(other): the old link is dropped first. */
				struct client_rpc release = {
					.type = CRPC_ATTACH_ENTITY,
					.payload.attach_entity = {
						.vehicle_id = (uint32_t)remote->mounted_vehicle_id,
						.attach = false,
					},
				};
				clin_process_direct_remote(&release);
			}
			remote->mounted = true;
			remote->mounted_vehicle_id = vehicle;
			remote->attach_applied++;
			struct client_rpc call = {
				.type = CRPC_ATTACH_ENTITY,
				.payload.attach_entity = {
					.vehicle_id = (uint32_t)vehicle,
					.attach = true,
				},
			};
			clin_process_direct_remote(&call);
		} break;
		case MP_EVENT_ENTITY_METADATA: {
			int32_t id = event->packet.payload.entity_metadata.entity_id;
			if(id == remote->entity_id) {
				/* mp-effects: the server also sends our own watcher to us
				 * (EntityTrackerEntry.java:110 sendPacketToTrackedPlayersAnd
				 * TrackedEntity); Beta's first-person fire overlay reads its
				 * flag 0 (Entity.isBurning -> getEntityFlag(0)). Route it to
				 * the host body like the local server's
				 * CRPC_LOCAL_PLAYER_FIRE. Sneak/riding bits are ours. */
				const struct mp_watchable_entry* own0 = metadata_find(
					&event->packet.payload.entity_metadata.metadata, 0, 0);
				if(own0) {
					struct client_rpc fire = {
						.type = CRPC_LOCAL_PLAYER_FIRE,
						.payload.local_player_fire = {
							.owner = 0,
							.on_fire = mp_decode_entity_metadata_flags(
								own0->value.byte_value).on_fire,
						},
					};
					clin_process_direct_remote(&fire);
				}
				break;
			}
			struct remote_entity* entry = remote_entity_find(remote, id);
			if(!entry)
				break;
			if(entry->kind == REMOTE_ENTITY_PLAYER
			   || entry->kind == REMOTE_ENTITY_MOB) {
				/* Beta's watcher byte 0 is generic Entity state. Share the
				 * player decoder with mobs; only fire has a mob client consumer. */
				/* MP visual-parity pass item 1: a remote player's sneak state is
				 * NOT carried by Packet18Animation's crouch code (animate==4 ->
				 * EntityPlayer.func_6420_o(), which is a literally EMPTY method on
				 * BOTH EntityOtherPlayerMP and EntityPlayerSP [740c583] -- a
				 * genuine Beta no-op, not a gap in this port; see client_
				 * interface.c's CRPC_ENTITY_ANIMATION case). The real signal is the
				 * generic Entity DataWatcher flag byte: index 0 type 0 (byte), bit
				 * 1 (0x02) [740c583 Entity.isSneaking -> getEntityFlag(1) ->
				 * dataWatcher.getWatchableObjectByte(0) & (1<<1)], synced to
				 * observers via this same Packet40 EntityMetadata whenever a real
				 * server applies a Packet19EntityAction.
				 *
				 * Animation-parity pass (2026-09-03) items 3+4 widen this SAME
				 * byte read to the other two bits Entity.setEntityFlag also
				 * writes in onEntityUpdate: bit 0 (0x01) onFire, bit 2 (0x04)
				 * isRiding [740c583 Entity.getEntityFlag/setEntityFlag,
				 * Entity.isBurning/isRiding]. Packet39AttachEntity is NOT the
				 * riding signal for an observer -- its one server call site
				 * (EntityPlayerMP.onUpdate) sends it only down the mounting
				 * player's OWN connection, never broadcast to trackers (grepped
				 * the full server tree: zero other construction sites) -- so
				 * this metadata byte is the only wire-faithful source, exactly
				 * like sneaking. */
				const struct mp_watchable_entry* e0 = metadata_find(
					&event->packet.payload.entity_metadata.metadata, 0, 0);
				if(e0) {
					// mp_entity_flags.h's mp_decode_entity_metadata_flags is
					// the same bit math this comment cites, factored out so
					// tests/test_mp_animation_parity_wire.c can golden-test
					// the decode directly.
					struct mp_entity_metadata_flags flags
						= mp_decode_entity_metadata_flags(e0->value.byte_value);
					struct client_rpc call = {
						.type = CRPC_ENTITY_FLAGS,
						.payload.entity_flags = {
							.entity_id = (uint32_t)id,
							.sneaking = flags.sneaking,
							.on_fire = flags.on_fire,
							.is_riding = flags.is_riding,
						},
					};
					clin_process_direct_remote(&call);
				}
				if(entry->kind == REMOTE_ENTITY_PLAYER)
					break;
			}
			if(entry->kind != REMOTE_ENTITY_MOB)
				break; /* no metadata consumer for non-mob/non-player kinds yet */
			apply_mob_metadata(entry, &event->packet.payload.entity_metadata.metadata);
			struct client_rpc call = {.type = CRPC_ENTITY_MOVE};
			call.payload.entity_move.entity_id = (uint32_t)id;
			call.payload.entity_move.pos[0]
				= (float)mp_fixed32_to_double(entry->server_x);
			call.payload.entity_move.pos[1]
				= (float)mp_fixed32_to_double(entry->server_y);
			call.payload.entity_move.pos[2]
				= (float)mp_fixed32_to_double(entry->server_z);
			call.payload.entity_move.hurt = entry->hurt_active;
			call.payload.entity_move.death_time = entry->death_time_sent;
			call.payload.entity_move.fleece_color = entry->fleece_color;
			call.payload.entity_move.sheared = entry->sheared;
			call.payload.entity_move.saddled = entry->saddled;
			call.payload.entity_move.wolf_flags = entry->wolf_flags;
			call.payload.entity_move.wolf_health = entry->wolf_health;
			call.payload.entity_move.charged = entry->charged;
			call.payload.entity_move.ghast_attacking = entry->ghast_attacking;
			clin_process_direct_remote(&call);
		} break;

		/* S4 P3 (misc/polish). */
		case MP_EVENT_CHAT: {
			struct client_rpc call = {.type = CRPC_CHAT_LINE};
			/* DISPLAY transcode, not UTF-8: the font renderer indexes bytes
			 * and parses single-byte § colour codes -- see
			 * mp_utf16_to_display's header comment. Live servers colour
			 * almost every line (/help, join messages, MOTD). */
			if(!mp_utf16_to_display(&event->packet.payload.chat,
									call.payload.chat_line.text,
									sizeof(call.payload.chat_line.text))) {
				/* Malformed UTF-16 (unpaired surrogate): "fails visibly"
				 * means a named counter, never a silent drop. [740c583
				 * Packet3Chat] */
				remote->chat_lines_rejected++;
				break;
			}
			remote->chat_lines_received++;
			clin_process_direct_remote(&call);
		} break;
		case MP_EVENT_NOTE_BLOCK: {
			/* instrumentType 0..4 map directly onto enum note_instrument's own
			 * values; anything else falls through to harp, exactly like
			 * Beta's own if-chain (no else/default that rejects). pitch is
			 * consumed as the raw 0..255 byte -- noteblock_pitch's formula is
			 * valid over that whole domain, not just the local note block's
			 * own 0..24 state range. [740c583 BlockNote.playBlock]
			 *
			 * Adversarial-review MINOR closure: Beta's own World.playNoteAt
			 * gates this entirely on `getBlockId(x,y,z) > 0` -- if the block
			 * isn't there client-side (stale/unloaded chunk), Beta plays
			 * NOTHING. Mirror that via clin_world_block_present (server_
			 * remote.c has no gstate access of its own; see that seam's own
			 * comment). [740c583 World.playNoteAt]
			 * Deviation, documented not fixed: Beta's playBlock also calls
			 * spawnParticle("note", ...) alongside the sound; this engine
			 * has no client-driven remote particle-spawn RPC yet, so that
			 * particle is dropped here -- cosmetic only, no simulation
			 * effect. [740c583 BlockNote.playBlock]
			 *
			 * mp-effects (2026-10-29): superseded. Packet54 is a generic
			 * BLOCK EVENT: World.playNoteAt calls
			 * Block.blocksList[id].playBlock(world,x,y,z,b1,b2) for whatever
			 * block is at (x,y,z) [740c583 World.playNoteAt:2339-2344]. The
			 * client seam dispatches on the client world's block: a note
			 * block also spawns its "note" particle (BlockNote.playBlock:69),
			 * a piston runs its own client-side extend/retract
			 * (BlockPistonBase.playBlock:82-170), anything else is a no-op
			 * (and no longer plays a harp note). The sound is emitted here. */
			int32_t nx = event->packet.payload.note_block.x;
			int32_t ny = event->packet.payload.note_block.y;
			int32_t nz = event->packet.payload.note_block.z;
			uint8_t b1 = event->packet.payload.note_block.instrument;
			uint8_t b2 = event->packet.payload.note_block.pitch;
			enum clin_block_event_result what
				= clin_remote_block_event(nx, ny, nz, b1, b2);
			struct client_rpc call = {
				.type = CRPC_SOUND,
				.payload.sound = {
					.pos = {(float)nx + 0.5f, (float)ny + 0.5f,
						(float)nz + 0.5f},
				},
			};
			if(what == CLIN_BLOCK_EVENT_NOTE) {
				switch(b1) {
					case 1: call.payload.sound.sound_id = SOUND_NOTE_BD; break;
					case 2: call.payload.sound.sound_id = SOUND_NOTE_SNARE; break;
					case 3: call.payload.sound.sound_id = SOUND_NOTE_HAT; break;
					case 4:
						call.payload.sound.sound_id = SOUND_NOTE_BASSATTACK;
						break;
					default: call.payload.sound.sound_id = SOUND_NOTE_HARP; break;
				}
				call.payload.sound.vol = 3.0f;
				call.payload.sound.pitch = noteblock_pitch(b2);
			} else if(what == CLIN_BLOCK_EVENT_PISTON_OUT) {
				/* "tile.piston.out" 0.5F, rand*0.25F+0.6F, only after
				 * tryExtend succeeded. The client's own unsynced roll, fixed
				 * at its midpoint like the explosion/door sounds here. */
				call.payload.sound.sound_id = SOUND_TILE_PISTON_OUT;
				call.payload.sound.vol = 0.5f;
				call.payload.sound.pitch = 0.725f;
			} else if(what == CLIN_BLOCK_EVENT_PISTON_IN) {
				/* "tile.piston.in" 0.5F, rand*0.15F+0.6F, unconditional. */
				call.payload.sound.sound_id = SOUND_TILE_PISTON_IN;
				call.payload.sound.vol = 0.5f;
				call.payload.sound.pitch = 0.675f;
			} else
				break; /* no chunk, air, or a block whose playBlock is empty */
			clin_process_direct_remote(&call);
		} break;
		case MP_EVENT_EXPLOSION: {
			double ex = event->packet.payload.explosion.x;
			double ey = event->packet.payload.explosion.y;
			double ez = event->packet.payload.explosion.z;
			/* Adversarial-review CRITICAL closure: this used to be
			 * (int32_t)floor(ex/ey/ez), but Beta's own reader does
			 * `int var3 = (int)this.explosionX` -- a Java (int) cast, i.e.
			 * TRUNCATE toward zero, not floor. The two diverge for any
			 * negative fractional origin (floor(-1.5)=-2, truncate=-1),
			 * exactly the trap CODESTYLE.md 2.3 calls out ("floor !=
			 * Java (int) cast for negatives"). java_double_to_int
			 * (worldgen/jmath.h) IS that cast -- NaN->0 and int32-clamped
			 * internally -- so the isfinite/range guard this replaces is
			 * now redundant for producing a defined int32; only isfinite
			 * is kept, to keep this handler's existing "fails visibly on
			 * malformed input" policy rather than silently accepting a
			 * NaN/Infinity origin as a clamped edge coordinate. [740c583
			 * Packet60Explosion.readPacketData] */
			if(!isfinite(ex) || !isfinite(ey) || !isfinite(ez)) {
				publish_error(remote, event->generation,
					"invalid remote explosion origin");
				mp_event_destroy(&event->packet);
				break;
			}
			int32_t base_x = java_double_to_int(ex);
			int32_t base_y = java_double_to_int(ey);
			int32_t base_z = java_double_to_int(ez);
			const struct mp_explosion_offset* blocks
				= event->packet.payload.explosion.blocks;
			uint16_t count = event->packet.payload.explosion.count;
			/* NetClientHandler.handleExplosion applies the destroyed-block
			 * list as a block clear (setBlockWithNotify(...,0)) through the
			 * same client path ordinary block changes use. [740c583
			 * NetClientHandler.handleExplosion] [740c583
			 * Explosion.doExplosionB] */
			for(uint16_t i = 0; i < count; i++) {
				int32_t by = base_y + blocks[i].dy;
				if(by < 0 || by > 127)
					continue; /* outside world height; never resident anyway */
				remote_apply_block_change(remote, base_x + blocks[i].dx,
					(uint8_t)by, base_z + blocks[i].dz, BLOCK_AIR, 0);
			}
			struct client_rpc explode = {
				.type = CRPC_ENTITY_EXPLODE,
				.payload.entity_explode = {
					.pos = {(float)ex, (float)ey, (float)ez},
					.power = event->packet.payload.explosion.power,
				},
			};
			clin_process_direct_remote(&explode);
			struct client_rpc sound = {
				.type = CRPC_SOUND,
				.payload.sound = {
					.sound_id = SOUND_RANDOM_EXPLODE,
					.pos = {(float)ex, (float)ey, (float)ez},
					.vol = 4.0f,
					/* Beta rolls this from the client's own LOCAL, unsynced
					 * java.util.Random each time [740c583
					 * Explosion.doExplosionB]; server_remote has no cosmetic
					 * RNG stream of its own, so the fixed midpoint of that
					 * roll (1.0 +/- 0.2, times 0.7) is used instead of adding
					 * one purely for this wobble -- never a simulation value,
					 * a documented simplification of a value Beta itself
					 * never synchronizes between clients. */
					.pitch = 0.7f,
				},
			};
			clin_process_direct_remote(&sound);
			mp_event_destroy(&event->packet);
		} break;
		case MP_EVENT_DOOR_CHANGE: {
			/* Beta's IWorldAccess.playAuxSFX fan-out covers ~20 numeric
			 * effect ids (door, fizz, extinguish, potion, jukebox start/stop,
			 * smoke, ...); this engine has no generic effect-id dispatch.
			 * 1003 (door open/close) is the one this packet is literally
			 * named for and already has a client sound hook (the local SP
			 * door path emits the SAME pair) -- reuse it, faithfully
			 * replicating that Beta's own client ALSO coin-flips the
			 * open/close FILE regardless of the real resulting state
			 * (block_door.c's own "FAITHFUL TRAP" note). Every other id is a
			 * counted, documented no-op; a future slice (jukebox record
			 * events, 1005) has a real hook to extend without re-touching
			 * server-side wiring. [740c583 Packet61DoorChange] [740c583
			 * World.playAuxSFX] [740c583 BlockDoor.blockActivated ->
			 * RenderGlobal.playAuxSFX case 1003]
			 *
			 * Adversarial-review MAJOR closure: this used to roll the
			 * open/close FILE and the pitch with bare libc rand() -- the
			 * only bare rand() call in this file, unseeded, and in direct
			 * tension with the explosion-sound case just above, which
			 * documents that "server_remote has no cosmetic RNG stream of
			 * its own." Same rationale, same fix: Beta rolls both from the
			 * client's own LOCAL, unsynced RNGs each time (Math.random()
			 * for the file, worldObj.rand for the pitch) [740c583
			 * RenderGlobal.playAuxSFX case 1003], so a fixed choice here is
			 * exactly as faithful as any other unsynced roll and costs
			 * nothing -- SOUND_RANDOM_DOOR_OPEN (an arbitrary but fixed
			 * pick of the coin flip) and pitch 0.95F (the midpoint of
			 * Beta's own nextFloat()*0.1F+0.9F range), never a simulation
			 * value.
			 *
			 * mp-effects (2026-10-29): superseded by remote_aux_sfx, the
			 * whole RenderGlobal effect table (1000-1005, 2000, 2001). The
			 * door now coin-flips open/close from a client-local cosmetic
			 * RNG as Beta's Math.random() does. Ids Beta's client ignores
			 * stay counted in door_change_unhandled. */
			if(!remote_aux_sfx(event->packet.payload.door_change.effect_id,
				   event->packet.payload.door_change.x,
				   event->packet.payload.door_change.y,
				   event->packet.payload.door_change.z,
				   event->packet.payload.door_change.data))
				remote->door_change_unhandled++;
		} break;
		case MP_EVENT_BED: {
			int8_t code = event->packet.payload.bed.event_code;
			if(code == 0) {
				/* tile.bed.notValid -- the ONE Beta string for this event,
				 * matching the existing local bed-fail HUD toast verbatim
				 * (server_local.c's own "P4(a)/(c)" citation). [740c583
				 * Packet70Bed] [740c583 NetClientHandler.handleBedEvent] */
				struct client_rpc call = {.type = CRPC_HUD_MESSAGE};
				snprintf(call.payload.hud_message,
					sizeof(call.payload.hud_message), "%s",
					"Your home bed was missing or obstructed");
				clin_process_direct_remote(&call);
			} else if(code == 1 || code == 2) {
				/* Rain state only; Beta's own handler never touches
				 * thundering either, and no other remote wire path sets
				 * weather in this slice -- thundering/thunder_strength stay
				 * permanently false/0 here (documented, not a silent
				 * clobber: nothing else drives them yet). [740c583
				 * NetClientHandler.handleBedEvent] */
				struct client_rpc call = {
					.type = CRPC_WEATHER_SET,
					.payload.weather_set = {
						.raining = (code == 1),
						.thundering = false,
						.rain_strength = (code == 1) ? 1.0f : 0.0f,
						.thunder_strength = 0.0f,
					},
				};
				clin_process_direct_remote(&call);
			} else
				remote->bed_events_unhandled++;
		} break;
		case MP_EVENT_WEATHER: {
			/* NetClientHandler.handleWeather: only type 1 spawns an
			 * EntityLightningBolt, at the fixed-point position / 32.0.
			 * Its first client onUpdate (lightningState == 2) plays
			 * "ambient.weather.thunder" 10000.0F, 0.8F + r*0.2F and
			 * "random.explode" 2.0F, 0.5F + r*0.2F at the bolt; restrikes
			 * reset the state to 1, so the pair plays exactly once per
			 * packet. b1.7.3 has no sound packet: this is the only way an
			 * online storm is heard. Cosmetic RNG (Beta's own unsynced
			 * entity Random). Fire and entity strikes are the server's.
			 * Drawn: handleWeather's addWeatherEffect(new
			 * EntityLightningBolt) becomes a client render bolt
			 * (graphics/render_lightning.c: RenderLightningBolt mesh +
			 * World.field_27172_i sky flash, render lifecycle only).
			 * [740c583 NetClientHandler.handleWeather]
			 * [740c583 EntityLightningBolt.onUpdate] */
			if(event->packet.payload.weather.is_lightning_bolt != 1) {
				remote->weather_lightning_noop++;
				break;
			}
			float bx = (float)mp_fixed32_to_double(event->packet.payload.weather.x);
			float by = (float)mp_fixed32_to_double(event->packet.payload.weather.y);
			float bz = (float)mp_fixed32_to_double(event->packet.payload.weather.z);
			struct random_gen* rng = audio_cosmetic_rng();
			float thunder_pitch = 0.8F + rand_gen_flt(rng) * 0.2F;
			aux_sfx_sound(SOUND_AMBIENT_WEATHER_THUNDER, bx, by, bz, 10000.0F,
				thunder_pitch);
			float explode_pitch = 0.5F + rand_gen_flt(rng) * 0.2F;
			aux_sfx_sound(SOUND_RANDOM_EXPLODE, bx, by, bz, 2.0F, explode_pitch);
			lightning_client_spawn((double)bx, (double)by, (double)bz,
				clin_remote_world_generation(),
				(int32_t)remote->dimension); /* the current remote dim */
			remote->weather_lightning_bolts++;
		} break;
		case MP_EVENT_MAP_DATA: {
			/* [740c583 NetClientHandler.handleMapData]: only
			 * `field_28055_a == Item.mapItem.shiftedIndex` (358, == this
			 * port's ITEM_MAP) routes to map data; any other item id just
			 * logs "Unknown itemid" and drops -- counted here instead of
			 * printed. */
			const int16_t item_id = event->packet.payload.map_data.item_id;
			const int16_t map_id = event->packet.payload.map_data.map_id;
			const uint8_t length = event->packet.payload.map_data.length;
			const uint8_t* bytes = event->packet.payload.map_data.bytes;
			if(item_id != ITEM_MAP) {
				remote->map_data_dropped++;
				break;
			}
			/* [740c583 MapData.updateMPMapData]: var1[0] selects the
			 * payload type. Beta indexes the array with NO length checks --
			 * a zero-length array, a type-0 shorter than 3 bytes, or a
			 * column/row past 127 all AIOOBE the vanilla client's read
			 * thread. A crash is not a portable behaviour: malformed input
			 * is dropped and counted (the S3 hostile-vector discipline). */
			if(length == 0) {
				remote->map_data_dropped++;
				break;
			}
			if(bytes[0] == 0) {
				if(length < 3) {
					remote->map_data_dropped++;
					break;
				}
				struct remote_map_mirror* mirror
					= remote_map_mirror_for(remote, map_id);
				if(!mirror) {
					if(remote->map_update_deferred != UINT32_MAX) remote->map_update_deferred++;
					return false;
				}
				/* column x = data[1] & 255, starting row = data[2] & 255
				 * and colors[(i + row) * 128 + x] =
				 * data[i + 3] for i in [0, length-3). Beta's client relies
				 * on the server never sending x/rows past 127; a hostile
				 * peer can, so out-of-plane writes are skipped (counted
				 * once as dropped, applied writes still land). */
				const uint8_t x = bytes[1];
				const uint8_t y0 = bytes[2];
				bool clamped = false;
				for(uint32_t i = 0; i + 3 < (uint32_t)length; i++) {
					const uint32_t row = i + y0;
					if(x >= MAP_DATA_DIM || row >= MAP_DATA_DIM) {
						clamped = true;
						continue;
					}
					mirror->colors[row * MAP_DATA_DIM + x] = bytes[i + 3];
				}
				if(clamped)
					remote->map_data_dropped++;
				/* markDirty() is unconditional after the loop in Beta --
				 * even a zero-write strip (length == 3) marks. The dirty
				 * mirror is pushed by flush_map_mirrors at pump tail. */
				mirror->dirty = true;
				remote->map_strips_applied++;
			} else if(bytes[0] == 1) {
				/* Packet131MapData.java:19-21; MapData.java:147-158.
				 * Pump/main thread shares the renderer's map cache. Retain the
				 * event for retry on allocation/spill failure, as for strips. */
				if(!clin_map_markers_deliver(map_id, bytes, length)) {
					if(remote->map_update_deferred != UINT32_MAX) remote->map_update_deferred++;
					return false;
				}
				remote->map_markers_applied++;
			} else {
				/* Beta has no else branch: unknown types fall through
				 * silently. Counted for observability; behaviour (no
				 * effect) is identical. */
				remote->map_data_dropped++;
			}
			break;
		}
		case MP_EVENT_STATISTIC: {
			/* NetClientHandler.handleStatistic ->
			 * EntityClientPlayerMP.func_27027_b(StatList.func_27361_a(id),
			 * amount): an unknown id is null and ignored; an "independent"
			 * (client-owned) stat is ignored; anything else goes through
			 * addStat (achievement parent check + toast, or the counter).
			 * The achievements consumer owns all of that by stat id
			 * (achievements.c GE_REMOTE_STAT: ach_index_of_stat_id, owner
			 * gate, parent rule), so this site only forwards. Beta's server
			 * never sends a negative amount (EntityPlayerMP.addStat splits
			 * into <= 100 chunks); the consumer would reject one, so it is
			 * counted here instead. The owner epoch is the remote session's
			 * (client_session_achievements_remote); 0 = not admitted, which
			 * the consumer drops as stale. [740c583
			 * NetClientHandler.handleStatistic] [740c583
			 * EntityClientPlayerMP.func_27027_b] */
			int32_t stat_id = event->packet.payload.statistic.stat_id;
			int32_t amount = event->packet.payload.statistic.amount;
			if(amount < 0 || stat_id < 0) {
				remote->statistic_noop++;
				break;
			}
			game_event_emit(&(struct game_event){
				.kind = GE_REMOTE_STAT,
				.authority = GE_AUTH_PACKET200,
				.who = {.slot = 0, .generation = game_event_owner_epoch(0)},
				.u.remote = {.stat_id = stat_id, .amount = amount},
			});
			remote->statistic_forwarded++;
		} break;

		case MP_EVENT_DISCONNECT:
			thread_mutex_lock(&remote->lock);
			/* DISPLAY transcode (kick reasons render on the disconnect
			 * screen; colour codes are common on public servers). */
			if(!mp_utf16_to_display(&event->packet.payload.disconnect_reason,
									remote->reason, sizeof(remote->reason)))
				set_reason_locked(remote, "invalid disconnect reason encoding");
			/* W6 (mp-split-0912/35): a Packet255 whose reason string is
			 * ZERO-LENGTH transcodes successfully to "" -- so the branch
			 * above leaves the terminal text empty, the disconnect screen
			 * falls back to the generic "Disconnected", and the heartbeat
			 * reports mp_end=1 with mp_end_reason="". A kick with no words is
			 * still a kick; name it rather than transcribing the silence. */
			if(remote->reason[0] == '\0')
				set_reason_locked(remote, "kicked by the server (no reason given)");
			/* GUARD 2's latch (mp-split-0912/5): the reason now on display
			 * came from the SERVER's Packet255. Set it for BOTH outcomes
			 * above -- a kick whose reason string was malformed is still a
			 * kick, and its "invalid disconnect reason encoding" text is
			 * still more truthful than the transport's follow-on EOF. */
			remote->reason_from_disconnect = true;
			remote->state = REMOTE_KICKED;
			remote->stop_now = true;
			close_socket_locked(remote);
			thread_cond_broadcast(&remote->write_signal);
			thread_mutex_unlock(&remote->lock);
			break;
	}
	return true;
}

/* hw1029b/G1: see server_remote.h. Same lock-guarded-copy shape as
 * server_remote_get_status; the counters themselves are single-writer. */
bool server_remote_get_kind_counts(struct server_remote* remote,
								   uint32_t out[SR_KIND_COUNT]) {
	memset(out, 0, sizeof(uint32_t) * SR_KIND_COUNT);
	if(!remote || !remote->impl)
		return false;
	struct server_remote_impl* impl = remote->impl;
	thread_mutex_lock(&impl->lock);
	memcpy(out, impl->kind_counts, sizeof(impl->kind_counts));
	thread_mutex_unlock(&impl->lock);
	return true;
}

#endif /* CAVEWII_MP_CLIENT */
