/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "server_remote_internal.h"

#ifdef CAVEWII_MP_CLIENT

/* Readiness provider registration only (server_remote_ready_census below is
 * installed into the loading screen's provider slot from server_remote_pump).
 * The header is declarations-only; the census type itself comes from our own
 * server_remote.h. */
#include "../game/gui/screen_load_world.h"
/* W4: mp_backing_stats for the status snapshot's budget high-water. */
#include "../platform/mem2.h"

/* Loading-screen stall-token rx provider (REL-MP-FIX4a): injected beside the
 * census so screen_load_world.c does not link this file. */
static uint32_t server_remote_rx_bytes(struct server_remote* remote) {
	struct server_remote_status st;
	memset(&st, 0, sizeof(st));
	server_remote_get_status(remote, &st);
	return st.rx_bytes;
}

#ifdef PLATFORM_WII
#include <stdio.h>
#include <stdlib.h>
// S5.3 hunt PROBE (temporary, round 2 cont.): brackets server_remote_pump's
// VERY FIRST lock attempt each call (main thread) -- if take 9's silence is
// the main thread blocked trying to acquire impl->lock (because the read
// thread stalled/faulted holding it -- see cw_lock_trace in
// server_remote_transport.c), this shows "pump_lock_before" with no
// matching "pump_lock_after" on the exact call that never returns. Own
// small per-call budget so it survives well past main.c's swap-window
// budget, which take 9 showed exhausts by frame n=4 -- before the death.
static void cw_pump_lock_trace(const char* label, uint32_t call_n) {
	static unsigned budget = 400;
	if(budget == 0 || CW_SD_TRACE_OFF())
		return;
	budget--;
	FILE* f = fopen("sd:/cw_boot_trace.txt", "a");
	if(!f)
		return;
	fprintf(f, "%s n=%lu\n", label, (unsigned long)call_n);
	fflush(f);
	fclose(f);
}
#endif

/* The pump/status/API seam: server_remote_pump (drains decoded events into
 * process_event, runs the map-mirror/entity-status pump tail and the
 * chunk-unload/promotion/load-completion machinery, then detects teardown
 * completion), server_remote_get_status/trace_dump, the test-only retained-
 * byte-cap setter, and remote_try_crpc -- the one CRPC-send primitive every
 * domain (protocol/entities/chunks) shares, so it lives here rather than in
 * any single domain module. Split 2026-08-26 (maintainer-register item 1)
 * out of the former monolithic ~3970-line server_remote.c; the transport,
 * protocol, entities and chunk-retention/promotion code that used to live
 * in this file now lives in server_remote_transport.c/_protocol.c/
 * _entities.c/_chunks.c respectively, sharing struct server_remote_impl (and
 * everything nested inside it) via server_remote_internal.h. See that
 * header's top comment for the full rationale. */


/* M8 (2026-08-20 redesign): per-frame cap on events processed by
 * server_remote_pump. 4 full queue refills a frame is generous catch-up
 * headroom; past it the tail waits for the next frame so a fast LAN peer
 * cannot hold the main thread (render/input/watchdog heartbeat) inside the
 * drain loop indefinitely. The read thread's backpressure (it parks on
 * event_space until this drain frees a slot, REL-MP-FIX5) covers the
 * producer side. */
#define REMOTE_PUMP_EVENT_BUDGET (REMOTE_QUEUE_CAPACITY * 4)

bool remote_try_crpc(struct server_remote_impl* remote,
							struct client_rpc* call, const char* failure) {
	if(clin_try_rpc_send(call))
		return true;
	/* Full CRPC inbox = ordinary flow control, not a protocol failure. The
	 * pump runs on the CLIENT thread immediately before clin_update() [plan
	 * 3.2], so the inbox can always be drained IN PLACE and the send
	 * retried -- a real server's login burst (dozens of entity spawns
	 * translated in one pump) routinely fills it, which used to kill every
	 * populated-server join here ("client spawn-monster inbox full",
	 * 2026-08-19 live finding; same instant-fatal-on-full class as
	 * push_event_locked's fix). Order is preserved: the inbox is FIFO and
	 * clin_update() consumes it fully before this send re-enqueues. Only a
	 * send that fails against a freshly drained inbox -- impossible for a
	 * bounded queue drained on this same thread -- remains fatal. M7: the
	 * DRAIN variant, not clin_update() -- the full update resets the
	 * per-frame dig-burst particle budget, and this path may run many
	 * times per frame. */
	clin_update_drain();
	if(clin_try_rpc_send(call))
		return true;
	publish_error(remote, remote->generation, failure);
	return false;
}

/* Bounded to 64 inline strips (16,512 bytes on Wii). On overflow, invalidate
 * the oldest pending identity, including its cached/spilled plane and queued
 * strips. This is explicit unknown state, not a successful strip application:
 * the next Packet131 for that id re-covers its addressed pixels. Beta has no
 * client re-request; untouched pixels remain unknown/blank until sent again.
 * Never put failed map work back at the shared transport FIFO's head. */
static void queue_map_retry(struct server_remote_impl* impl,
                            const struct remote_event* event) {
	if(impl->map_retry_count == REMOTE_MAP_RETRIES) {
		int16_t id = impl->map_retries[0].map_id;
		unsigned count = 0;
		for(unsigned i = 0; i < impl->map_retry_count; i++)
			if(impl->map_retries[i].map_id != id)
				impl->map_retries[count++] = impl->map_retries[i];
		impl->map_retry_count = count;
		for(unsigned i = 0; i < REMOTE_MAP_MIRRORS; i++)
			if(impl->map_mirrors[i].used && impl->map_mirrors[i].map_id == id)
				memset(&impl->map_mirrors[i], 0, sizeof(impl->map_mirrors[i]));
		clin_map_forget(id);
		if(impl->map_retry_overflows != UINT32_MAX) impl->map_retry_overflows++;
	}
	struct remote_map_retry* retry = &impl->map_retries[impl->map_retry_count++];
	retry->map_id = event->packet.payload.map_data.map_id;
	retry->length = event->packet.payload.map_data.length;
	memcpy(retry->bytes, event->packet.payload.map_data.bytes, retry->length);
}

static bool map_retry_pending(struct server_remote_impl* impl, int16_t id) {
	for(unsigned i = 0; i < impl->map_retry_count; i++)
		if(impl->map_retries[i].map_id == id) return true;
	return false;
}

static void retry_map_strips(struct server_remote* remote) {
	struct server_remote_impl* impl = remote->impl;
	unsigned count = impl->map_retry_count;
	impl->map_retry_count = 0;
	for(unsigned i = 0; i < count; i++) {
		struct remote_map_retry retry = impl->map_retries[i];
		struct remote_event event = {.kind = REMOTE_EVENT_PACKET,
			.generation = impl->generation};
		event.packet.type = MP_EVENT_MAP_DATA;
		event.packet.payload.map_data.item_id = ITEM_MAP;
		event.packet.payload.map_data.map_id = retry.map_id;
		event.packet.payload.map_data.length = retry.length;
		memcpy(event.packet.payload.map_data.bytes, retry.bytes, retry.length);
		if(map_retry_pending(impl, retry.map_id) || !process_event(remote, &event))
			impl->map_retries[impl->map_retry_count++] = retry;
	}
}

/* push-0921/B2 observation seam: how many remote world installs have
 * completed IN THIS PROCESS -- the design's connection generation (design
 * tmp/jobs/push-0921/design-mp-play-10m.md section 2). PROCESS-global and
 * NOT a field of the impl or of the outer handle, for the same reason
 * mp_backing_stats' numbers in the snapshot below are: a rejoin runs
 * mp_session_release_remote (server_remote_destroy, the impl is freed) and
 * then mp_session_prepare_remote (server_remote_init, a new impl), and
 * telling gen 1 from gen 2 ACROSS exactly that is the whole point of the
 * counter -- a per-impl field would restart at 0 on the rejoin it exists to
 * number. Written only from server_remote_pump (main thread), read only into
 * the status snapshot. Saturates rather than wrapping: a generation that
 * went backwards, or back to the "no install yet" 0, would silently credit a
 * stale receipt. */
static uint32_t s_world_install_generation;

void server_remote_pump(struct server_remote* remote) {
	/* M3 readiness provider (screen_load_world.h explains the inversion: an
	 * engine-core screen may not hard-reference an MP-pool symbol). Armed and
	 * disarmed HERE rather than at init/destroy because the pump is the one
	 * call that runs exactly while a session has a live impl, which makes the
	 * invariant self-maintaining: the loading screen can never hold a
	 * provider that would dereference a freed handle. A stale arm is harmless
	 * anyway -- the remote branch is gated on gstate.remote_session and the
	 * census itself refuses a NULL impl. */
	if(!remote || !remote->impl) {
		lworld_set_ready_census_provider(NULL, NULL, NULL);
		return;
	}
	/* ARM/DISARM PLACEMENT IS LOAD-BEARING (2026-09-13 deadlock fix, F3).
	 * The slot must be armed only across the gap BETWEEN pumps -- the part of
	 * the frame where this thread holds no transport lock -- so it is cleared
	 * on the way in and installed on the way out, after the pump's last
	 * unlock (bottom of this function). It is NOT merely a defensive window.
	 *
	 * Arming it here (as the first W3 land did) self-deadlocked the main
	 * thread on the very first remote world load. The pump's tail holds
	 * impl->lock across remote_emit_load_completion_locked
	 * (server_remote_chunks.c), which delivers the FINISH_REMOTE_LOAD
	 * CRPC_PLAYER_POS SYNCHRONOUSLY via clin_process_direct; that handler
	 * runs screen_set(&screen_ingame) -> lworld_admit_gameplay ->
	 * lworld_prepare_presentation, whose remote branch then called the armed
	 * provider -- server_remote_ready_census -- which re-locks the same
	 * non-recursive impl->lock this thread already owns
	 * (source/platform/thread.c's plain pthread_mutex_lock). Observed stack:
	 * server_remote_pump -> remote_emit_load_completion_locked ->
	 * clin_process_direct -> clin_process_observed -> screen_set ->
	 * lworld_admit_gameplay -> lworld_prepare_presentation ->
	 * server_remote_ready_census -> thread_mutex_lock == permanent hang
	 * (tests/test_mp_s0_env.c's test_mc173_partial_join_burst is the
	 * end-to-end guard: it wedged forever at d75443fb).
	 *
	 * Keeping the slot clear for the WHOLE body -- not just around that one
	 * emit -- is deliberate: every synchronous client-RPC delivery the pump
	 * makes (remote_emit_pending_unloads_locked, the promotion step's
	 * CRPC_CHUNK publications, process_event) can re-enter the screen the
	 * same way, and the pump has exactly one other exit (the !impl return
	 * above), so "clear on entry, install on exit" is checkable by reading
	 * this function rather than by auditing every callee's lock state. The
	 * gate's defined answer inside that window is "no census" -> never ready
	 * on the ledger's word, admitting at the backstop only if the CLIENT's
	 * own world already holds the safe core (lworld_remote_ready). */
	lworld_set_ready_census_provider(NULL, NULL, NULL);
	struct server_remote_impl* impl = remote->impl;
#ifdef PLATFORM_WII
	static uint32_t cw_pump_call_n;
	cw_pump_call_n++;
	cw_pump_lock_trace("pump_lock_before", cw_pump_call_n);
#endif
	thread_mutex_lock(&impl->lock);
#ifdef PLATFORM_WII
	cw_pump_lock_trace("pump_lock_after", cw_pump_call_n);
#endif
	impl->inflated_bytes_frame = 0;
	/* IN-009 step 7 LATCH TIMING. An error already sitting in the side-slot
	 * when this pump starts is latched HERE, before the drain below runs any
	 * queued event: a queued login/respawn would otherwise move the phase,
	 * and more received bytes would move rx, so the latch would describe a
	 * consequence instead of the moment the error was observed. First-wins
	 * (remote_latch_protocol_end_locked is a no-op once any code is set), so
	 * an EOF/receive/connect/capacity/kick code recorded earlier keeps its
	 * phase and rx. An error published DURING the drain is still latched at
	 * the side-slot below, at the point it is first observed. (Classifying
	 * at the producer is server_remote_transport.c's job: out of scope.) */
	if(impl->error_pending && impl->error_pending_generation == impl->generation
	   && !impl->graceful_stop && impl->state != REMOTE_KICKED)
		remote_latch_protocol_end_locked(impl);
	/* Pump-cadence watermark (reconnect-burst diagnostics, 2026-08-27): the
	 * overflow kill can only fire across a pump gap longer than the read
	 * thread's patience, so record the worst live-session gap where the
	 * kill's reason (and F3/status readers) can see it. */
	{
		ptime_t now = time_get();
		if(impl->state != REMOTE_DISCONNECTED) {
			if(impl->have_last_pump) {
				int32_t gap = time_diff_ms(impl->last_pump_time, now);
				if(gap > 0 && (uint32_t)gap > impl->pump_gap_max_ms)
					impl->pump_gap_max_ms = (uint32_t)gap;
			}
			impl->last_pump_time = now;
			impl->have_last_pump = true;
		} else
			impl->have_last_pump = false;
	}
	thread_mutex_unlock(&impl->lock);
	/* S5.3 hunt PROBE (temporary, ROUND 10 DISCRIMINATOR): fire the deferred
	 * login send once its ~3s hold expires (server_remote_protocol.c's
	 * MP_EVENT_HANDSHAKE case sets login_delay_pending instead of sending
	 * immediately -- see server_remote_internal.h's field comment for the
	 * full rationale). Polled here because process_event only runs per
	 * DEQUEUED event, not per frame; server_remote_pump runs every frame
	 * regardless of whether anything is queued, so this is the natural
	 * per-frame check. Same encode/queue/state-transition the immediate
	 * path used to do inline. */
	thread_mutex_lock(&impl->lock);
	bool login_due
		= impl->login_delay_pending && time_reached(time_get(),
													 impl->login_delay_deadline);
	if(login_due)
		impl->login_delay_pending = false;
	thread_mutex_unlock(&impl->lock);
	if(login_due) {
		struct mp_frame login;
		if(mp_encode_login(&login, impl->username) && queue_frame(impl, &login)) {
			thread_mutex_lock(&impl->lock);
			impl->state = REMOTE_LOGIN_SENT;
			thread_mutex_unlock(&impl->lock);
		} else {
			publish_error(impl, impl->generation, "failed to queue delayed login");
		}
	}
	/* Main-thread provider seam: consume the same mailbox as server_local. */
	svin_process_messages(remote_process_srpc, remote, false);
#ifdef PLATFORM_WII
	// S5.3 hunt PROBE (round 4 cont.): pump_lock_before/after only bracket
	// the FIRST lock above -- take 18 showed the death can happen before
	// EVEN reaching process_event(LOGIN) despite the read thread having
	// already decoded all 3 known packets cleanly (mc173 never sends a 4th
	// -- MAP_CHUNK/PRE_CHUNK are NEVER reached in any take, refuting the
	// "wild write in chunk decode" hypothesis directly). These markers
	// bracket the REST of this function's body to localize within it.
	cw_pump_lock_trace("pump_post_svin", cw_pump_call_n);
#endif
	/* M8: bounded drain -- see REMOTE_PUMP_EVENT_BUDGET's comment. */
	retry_map_strips(remote);
	uint32_t processed = 0;
	for(; processed < REMOTE_PUMP_EVENT_BUDGET; processed++) {
		struct remote_event event;
		thread_mutex_lock(&((struct server_remote_impl*)remote->impl)->lock);
		bool have = impl->event_count != 0;
		if(have) event = impl->events[impl->event_head];
		thread_mutex_unlock(&((struct server_remote_impl*)remote->impl)->lock);
		if(!have)
			break;
		thread_mutex_lock(&((struct server_remote_impl*)remote->impl)->lock);
		bool current_generation
			= event.generation
			  == ((struct server_remote_impl*)remote->impl)->generation;
		thread_mutex_unlock(&((struct server_remote_impl*)remote->impl)->lock);
		if(current_generation) {
			/* The edits this packet applies are stamped from its receive
			 * sequence (remote_deferred_edit_record_locked). Pump-thread-only
			 * fields, no lock needed. */
			impl->pump_packet_seq = event.seq;
			impl->pump_packet_sub = 0;
			bool map = event.kind == REMOTE_EVENT_PACKET
				&& event.packet.type == MP_EVENT_MAP_DATA
				&& event.packet.payload.map_data.item_id == ITEM_MAP;
			if((map && map_retry_pending(impl, event.packet.payload.map_data.map_id))
			   || !process_event(remote, &event))
				queue_map_retry(impl, &event);
		}
		else
			/* Stale-generation drop still owns the queued event's heap
			 * arrays (re-review finding 11) -- destroy, don't leak. */
			mp_event_destroy(&event.packet);
		thread_mutex_lock(&impl->lock);
		/* Only this main-thread pump consumes events; producer appends at tail. */
		impl->event_head = (impl->event_head + 1) % REMOTE_QUEUE_CAPACITY;
		/* REL-MP-FIX5: pump progress wakes a reader parked on the full ring
		 * (push_event_locked); signalled under `lock`, so never lost. */
		if(impl->event_count-- == REMOTE_QUEUE_CAPACITY)
			thread_cond_signal(&impl->event_space);
		thread_mutex_unlock(&impl->lock);
	}
	if(processed == REMOTE_PUMP_EVENT_BUDGET) {
		thread_mutex_lock(&impl->lock);
		/* Count only frames that actually left tail behind (re-review
		 * finding 7's off-by-one: draining exactly the budget with an
		 * empty queue after is a full drain, not a deferral). */
		if(impl->event_count > 0)
			impl->events_deferred_frames++;
		thread_mutex_unlock(&impl->lock);
	}
#ifdef PLATFORM_WII
	cw_pump_lock_trace("pump_post_drain", cw_pump_call_n);
#endif
	/* Flush after the drain, once per dirty identity. Failed delivery retains
	 * dirty bytes for the next pump, including pumps with no new strips. */
	flush_map_mirrors(impl);
#ifdef PLATFORM_WII
	cw_pump_lock_trace("pump_post_map_mirrors", cw_pump_call_n);
#endif
	/* Packet38 cosmetic clock: hurt-flash expiry + death-roll advance
	 * (see flush_entity_status). */
	flush_entity_status(impl);
#ifdef PLATFORM_WII
	cw_pump_lock_trace("pump_post_entity_status", cw_pump_call_n);
#endif
	/* Error side-slot synthesis (finding 4) -- after the drain pass so an
	 * error published DURING this frame's processing is still delivered
	 * this frame, with zero waiting from whichever thread published it. */
	{
		struct remote_event error_event;
		bool have_error = false;
		thread_mutex_lock(&impl->lock);
		if(impl->error_pending
		   && impl->error_pending_generation == impl->generation) {
			memset(&error_event, 0, sizeof(error_event));
			error_event.kind = REMOTE_EVENT_ERROR;
			error_event.generation = impl->error_pending_generation;
			snprintf(error_event.error, sizeof(error_event.error), "%s",
					 impl->error_pending_msg);
			have_error = true;
			/* IN-009 step 7: a terminal error no producer classified (EOF,
			 * receive, connect, capacity and kick latch their own codes
			 * first) still says HOW the session ended: mp_end 6, first-wins.
			 * A decoded Packet255 keeps its authority (the latch is a no-op
			 * under terminal_reason_pending / reason_from_disconnect), and a
			 * graceful local stop is not a failure. An error that was
			 * already pending at pump entry was latched there (see the top
			 * of this function); this call is a no-op for it and only
			 * latches an error first published during this pump's drain. */
			if(!impl->graceful_stop && impl->state != REMOTE_KICKED)
				remote_latch_protocol_end_locked(impl);
		}
		impl->error_pending = false;
		thread_mutex_unlock(&impl->lock);
		if(have_error)
			process_event(remote, &error_event);
	}
#ifdef PLATFORM_WII
	cw_pump_lock_trace("pump_post_error_slot", cw_pump_call_n);
#endif
	thread_mutex_lock(&impl->lock);
#ifdef PLATFORM_WII
	cw_pump_lock_trace("pump_tail_lock_after", cw_pump_call_n);
#endif
	remote_emit_pending_unloads_locked(impl);
#ifdef PLATFORM_WII
	cw_pump_lock_trace("pump_post_unloads", cw_pump_call_n);
#endif
	if(impl->state == REMOTE_PLAY && !impl->stop_now) {
		(void)remote_promotion_step_locked(impl);
		/* W1 demote-on-exit (design section 2.3): immediately after the
		 * promotion step, in the same guard -- a session that is not PLAYing
		 * or is already stopping has no residency to manage, and the
		 * promotion step may have just killed the connection, which
		 * remote_residency_step_locked's own stop_now check then respects. */
		remote_residency_step_locked(impl);
	}
#ifdef PLATFORM_WII
	cw_pump_lock_trace("pump_post_promotion", cw_pump_call_n);
#endif
	/* push-0921/B2 observation seam (recording only, design section 6): the
	 * false->true edge of remote_load_complete_sent IS "a remote world
	 * install completed" -- this pump call is the one and only caller of the
	 * emit that sets it (server_remote_chunks.c), so the edge is observed
	 * here rather than by adding a counter inside the emit. The count is
	 * PROCESS-global (see s_world_install_generation) because the rejoin it
	 * exists to number frees this impl and builds another one. */
	bool install_was_complete = impl->remote_load_complete_sent;
	remote_emit_load_completion_locked(impl);
	if(!install_was_complete && impl->remote_load_complete_sent
	   && s_world_install_generation != UINT32_MAX)
		s_world_install_generation++;
#ifdef PLATFORM_WII
	cw_pump_lock_trace("pump_post_load_completion", cw_pump_call_n);
#endif
	/* Teardown COMPLETION detection (re-review follow-up): nothing used to
	 * move CLOSING/KICKED (or a killed-in-PLAY session) to DISCONNECTED --
	 * a clean quit-to-title sat in CLOSING until the seam's 15s deadline
	 * belt fired. Once a stop is committed and both workers have exited,
	 * the session is fully down: finalize so the seam restores
	 * server_local on the next frame. */
	if((impl->stop_now || impl->graceful_stop)
	   && impl->state != REMOTE_DISCONNECTED
	   && (!impl->read_thread_started || impl->read_thread_exited)
	   && (!impl->write_thread_started || impl->write_thread_exited))
		impl->state = REMOTE_DISCONNECTED;
	thread_mutex_unlock(&impl->lock);
	/* The pump's last unlock is immediately above: from here to the next
	 * pump entry this thread holds no transport lock, which is exactly the
	 * window in which the frame's screen update evaluates the readiness gate.
	 * Install the provider for that window only -- see the entry comment for
	 * the deadlock this ordering fixes. */
	lworld_set_ready_census_provider(server_remote_ready_census,
									 server_remote_rx_bytes, remote);
}

/* mp-split-0912/7: latch the LIVE status into the outer handle so it survives
 * the impl being freed. Called from server_remote_destroy before its
 * server_remote_cancel/free, so `state` is the terminal state the impl
 * actually reached (DISCONNECTED once the pump finalized, or KICKED/CLOSING
 * if the caller tore the session down mid-teardown) and the mp_end/_phase/_rx
 * latch, generation, logged_in, rx_bytes and reason are the real ones.
 *
 * A handle whose impl never ran a connect attempt has generation 0 -- there
 * is no session to remember, so nothing is latched and get_status keeps its
 * zero fill (a heartbeat must not be able to report a session that never
 * existed). Every user-reachable start, INCLUDING a rejected one, bumps the
 * generation, so "a connect was attempted" and "generation != 0" are the
 * same statement. */
void server_remote_status_latch_capture(struct server_remote* remote) {
	if(!remote || !remote->impl)
		return;
	struct server_remote_status live;
	server_remote_get_status(remote, &live);
	if(live.generation == 0)
		return;
	/* memcpy, not struct assignment, and likewise on the way out in
	 * get_status: a struct assignment need not copy padding, so a snapshot
	 * taken that way could not be proved complete by a whole-struct memcmp
	 * (which is exactly how test_mp_s0_env pins "all fields survive, not
	 * just the handful the heartbeat happens to print"). */
	memcpy(&remote->last_status, &live, sizeof(remote->last_status));
	remote->last_status_valid = true;
}

void server_remote_status_latch_clear(struct server_remote* remote) {
	if(!remote)
		return;
	/* memset as well as clearing the flag: production places this handle in
	 * uninitialized automatic storage (see server_remote_init's comment), so
	 * the clear called from init is what keeps a torn stack-garbage snapshot
	 * from ever being inspected. */
	memset(&remote->last_status, 0, sizeof(remote->last_status));
	remote->last_status_valid = false;
}

void server_remote_get_status(struct server_remote* remote,
							  struct server_remote_status* status) {
	if(!status)
		return;
	memset(status, 0, sizeof(*status));
	/* push-0921/B18 (review B17 H2): world_install_generation used to be
	 * stamped here, for every one of this function's six non-play callers.
	 * It moved to struct server_remote_play_obs with the rings it belongs to,
	 * and server_remote_get_play_obs stamps it under the SAME rule -- process
	 * state, not session state, so it survives a destroyed session. */
	if(!remote || !remote->impl) {
		/* mp-split-0912/7: a destroyed session still answers with WHAT
		 * HAPPENED until the next connect replaces the impl. Without this a
		 * heartbeat sampled even one tick after mp_session_release_remote
		 * reported zeros, making a 400 ms kick indistinguishable from "MP
		 * was never attempted" in an unattended hardware run. */
		if(remote && remote->last_status_valid) {
			memcpy(status, &remote->last_status, sizeof(*status));
			/* The install count USED TO be re-stamped here, after the memcpy,
			 * because the latch is a record of the DEAD SESSION while the
			 * count is a record of the PROCESS. That distinction still holds
			 * and is still honoured -- it just lives in
			 * server_remote_get_play_obs now (push-0921/B18, review B17 H2),
			 * which reads s_world_install_generation live on every call and
			 * therefore cannot serve a snapshot-of-a-snapshot either. */
			return;
		}
		status->state = REMOTE_DISCONNECTED;
		return;
	}
	struct server_remote_impl* impl = remote->impl;
	thread_mutex_lock(&impl->lock);
	status->state = impl->state;
	status->generation = impl->generation;
	status->logged_in = impl->logged_in;
	status->graceful = impl->graceful_stop;
	status->entity_id = impl->entity_id;
	status->map_seed = impl->map_seed;
	status->dimension = impl->dimension;
	status->socket_open = impl->socket != NET_SOCKET_INVALID;
	status->socket_close_deferred
		= impl->pending_close_socket != NET_SOCKET_INVALID;
	status->read_thread_started = impl->read_thread_started;
	status->write_thread_started = impl->write_thread_started;
	status->read_thread_exited = impl->read_thread_exited;
	status->write_thread_exited = impl->write_thread_exited;
	status->queued_events = impl->event_count;
	thread_mutex_lock(&impl->frame_lock); /* REL-MP-FIX3e: writer-owned */
	status->queued_frames = impl->frame_count;
	status->keep_alives_sent = impl->keep_alives_sent;
	status->corrections_echoed = impl->corrections_echoed;
	thread_mutex_unlock(&impl->frame_lock);
	status->keep_alives_received = impl->keep_alives_received;
	status->retained_bytes = impl->retained_bytes;
	status->retained_bytes_peak = impl->retained_bytes_peak;
	status->retained_byte_cap = impl->retained_byte_cap;
	status->retained_entries = impl->retained_entries;
	status->retained_entry_peak = impl->retained_entry_peak;
	status->retained_entry_cap = REMOTE_CHUNK_ENTRY_CAPACITY;
	status->inflated_bytes_frame = impl->inflated_bytes_frame;
	status->retention_evictions = impl->retention_evictions;
	status->retention_allocation_failures
		= impl->retention_allocation_failures;
	status->reconnect_recoveries = impl->reconnect_recoveries;
	status->deferred_edits = impl->deferred_edits;
	status->deferred_edits_applied = impl->deferred_edits_applied;
	status->deferred_edits_dropped = impl->deferred_edits_dropped;
	status->forced_reconnects = impl->forced_reconnects;
	/* mp-split-0912/5: plain lock-guarded copies -- every writer of these
	 * four (the read thread's terminal latch, the pump's error-drop guard)
	 * holds impl->lock, unlike the relaxed-atomic diagnostics below. */
	status->errors_dropped_after_kick = impl->errors_dropped_after_kick;
	status->mp_end = impl->mp_end;
	status->mp_end_phase = impl->mp_end_phase;
	status->mp_end_rx = impl->mp_end_rx;
	/* Fix B (2026-08-31): these used to be plain reads of read_decoder's
	 * own fields and of rx_bytes ("torn reads acceptable, diagnostic
	 * only") while the read thread mutated them with NO synchronization at
	 * all -- genuine ISO C data-race UB, not merely a torn-read curiosity.
	 * Both sides now use relaxed C11 atomics (see the fields' declaration
	 * comments in server_remote_internal.h): the access itself is well-
	 * defined, the values are still best-effort/diagnostic-only exactly as
	 * before, and this whole function already holds impl->lock, so the
	 * atomic ops below add no new blocking beyond a relaxed load. */
	status->dec_phase = atomic_load_explicit(&impl->dec_phase_sample,
											 memory_order_relaxed);
	status->dec_packet_id = atomic_load_explicit(&impl->dec_packet_id_sample,
												 memory_order_relaxed);
	status->dec_chunk_have = atomic_load_explicit(
		&impl->dec_chunk_have_sample, memory_order_relaxed);
	status->dec_chunk_need = atomic_load_explicit(
		&impl->dec_chunk_need_sample, memory_order_relaxed);
	status->rx_bytes
		= atomic_load_explicit(&impl->rx_bytes, memory_order_relaxed);
	status->dying = impl->stop_now;
	status->event_backpressure_waits = impl->event_backpressure_waits;
	status->events_deferred_frames = impl->events_deferred_frames;
	status->queued_events_peak = impl->queued_events_peak;
	status->pump_gap_max_ms = impl->pump_gap_max_ms;
	status->corrections_received = impl->corrections_received;
	status->correction_seq = impl->correction_seq;
	status->climate_create_ms = impl->climate_create_ms;
	status->remote_entity_count = impl->remote_entities.count;
	status->remote_entity_capacity_drops = impl->remote_entities.capacity_drops;
	status->chat_lines_received = impl->chat_lines_received;
	status->chat_lines_sent = impl->chat_lines_sent;
	status->attach_applied = impl->attach_applied;
	status->attach_ignored = impl->attach_ignored;
	status->chat_lines_rejected = impl->chat_lines_rejected;
	status->door_change_unhandled = impl->door_change_unhandled;
	status->bed_events_unhandled = impl->bed_events_unhandled;
	status->weather_lightning_noop = impl->weather_lightning_noop;
	status->map_strips_applied = impl->map_strips_applied;
	status->map_markers_applied = impl->map_markers_applied;
	status->map_data_dropped = impl->map_data_dropped;
	status->map_mirror_evictions = impl->map_mirror_evictions;
	status->map_pushes = impl->map_pushes;
	status->map_push_deferred = impl->map_push_deferred;
	status->statistic_noop = impl->statistic_noop;
	/* push-0921/B18 (review B17 H2): the three observation rings USED TO be
	 * copied here, into this struct, on every one of this function's six
	 * non-play call sites -- 560 B of play-driver state on main()'s own frame,
	 * on every heartbeat and on every frame of the connect screen. They now
	 * live in struct server_remote_play_obs and are read through
	 * server_remote_get_play_obs below, whose only caller is the mp_play_10m
	 * hook. The WRITER side is unchanged: process_event still records into
	 * impl->block_echoes / txn_results / set_slots under this same lock. */
	/* W1 admission/residency (design sections 2.1-2.3). */
	status->admit_radius = impl->admit_radius;
	status->demotions = impl->demotions;
	status->demote_bytes = impl->demote_bytes;
	status->demotion_failures = impl->demotion_failures;
	/* W4 (design 2.7): the rest of the admission-budget counters. */
	status->compactions = impl->compactions;
	status->backing_refusals = impl->backing_refusals;
	/* F4 memory-aware promotion: a declared-but-never-filled field would be
	 * a silent zero, so the mirror lands here with the rest of the W1 set. */
	status->promote_deferred_memory = impl->promote_deferred_memory;
	/* F5b (H1, H4, M7): the hard-cap count and the re-encode measurement.
	 * resident_columns is the maintained `installed` count, NOT the
	 * RESIDENT-state tally the loop below builds: a column whose entry went
	 * back to RETAINED because a new body arrived still HOLDS its voxels, so
	 * the state tally understates the carve. Both are reported -- they are
	 * different questions and the earlier slices' claims were read off the
	 * wrong one. */
	status->resident_columns = impl->resident_cols;
	status->resident_columns_peak = impl->resident_cols_peak;
	status->promote_deferred_cap = impl->promote_deferred_cap;
	status->demote_deflate_us_last = impl->demote_deflate_us_last;
	status->demote_deflate_us_max = impl->demote_deflate_us_max;
	status->recode_aborts = impl->recode_aborts;
	status->unload_refused_retries = impl->unload_refused_retries;
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++) {
		if(impl->chunks[i].state == REMOTE_CHUNK_RETAINED)
			status->ready_entries++;
		if(impl->chunks[i].state == REMOTE_CHUNK_RETAINED
		   || impl->chunks[i].state == REMOTE_CHUNK_PROMOTING)
			status->promotion_backlog++;
		if(impl->chunks[i].state == REMOTE_CHUNK_RESIDENT)
			status->resident_entries++;
	}
	/* BACKED = the column is held only as a compressed body. That is exactly
	 * the promotion backlog's membership (RETAINED + PROMOTING -- a
	 * PROMOTING column's voxels are not installed yet either), so it is the
	 * same count under a name that says what the MEMORY tier is rather than
	 * what the pump is about to do with it. Kept as its own field instead of
	 * letting the heartbeat reuse mp_backlog: the two are equal today
	 * because W2 made TOMBSTONE unreachable, and a reader of the budget
	 * evidence must not silently inherit that coincidence. */
	status->backed_entries = status->promotion_backlog;
	snprintf(status->reason, sizeof(status->reason), "%s", impl->reason);
	thread_mutex_unlock(&impl->lock);
	/* mp_backing_stats takes the backing allocator's own lock, so it is
	 * sampled AFTER impl->lock is dropped: a body is allocated on the read
	 * thread (which can be inside the backing lock) while the pump holds
	 * impl->lock, so acquiring them in that order here would be the
	 * inverse nesting. The numbers are a process-global high-water rather
	 * than part of the entry-table snapshot above, so nothing is lost by
	 * reading them a moment later. */
	size_t backing_in_use = 0, backing_peak = 0, backing_capacity = 0;
	mp_backing_stats(&backing_in_use, &backing_peak, &backing_capacity);
	status->backing_bytes = backing_in_use;
	status->backing_peak_bytes = backing_peak;
	status->backing_capacity_bytes = backing_capacity;
}

/* push-0921/B18 (review B17 H2). Contract in server_remote.h. Deliberately a
 * sibling of server_remote_get_status rather than a flag on it: the six
 * non-play status readers must not be able to opt INTO this cost by accident,
 * and the one caller that does want it says so by name.
 *
 * Takes impl->lock, like get_status and for the same reason -- the records are
 * pump-thread-exclusive and the lock is what the transport's own writers hold.
 * No allocation and no writes to the impl: a pure reader. */
void server_remote_get_play_obs(struct server_remote* remote,
								struct server_remote_play_obs* out) {
	if(!out)
		return;
	memset(out, 0, sizeof(*out));
	/* PROCESS state, not session state, so it is stamped BEFORE the impl check
	 * and survives a destroyed session -- the same rule, for the same reason,
	 * server_remote_get_status applied to it while it lived there. A driver
	 * matching receipts by generation must tell gen 1 from gen 2 across a
	 * rejoin, and a rejoin frees the impl. */
	out->world_install_generation = s_world_install_generation;
	if(!remote || !remote->impl)
		return; /* left zero-filled: serial 0 already means "never written",
				 * so an impl-less read is indistinguishable from a fresh
				 * impl's -- never a torn or stale record. */
	struct server_remote_impl* impl = remote->impl;
	thread_mutex_lock(&impl->lock);
	out->block_echo_serial = impl->block_echo_serial;
	memcpy(out->block_echoes, impl->block_echoes, sizeof(out->block_echoes));
	out->txn_serial = impl->txn_serial;
	memcpy(out->txn_results, impl->txn_results, sizeof(out->txn_results));
	out->set_slot_serial = impl->set_slot_serial;
	memcpy(out->set_slots, impl->set_slots, sizeof(out->set_slots));
	/* push-0921/B19: the watched cell's slot. Zero (serial 0 = none) whenever
	 * no script has armed a watch, which is every other profile and all of
	 * normal play. */
	out->watch_echo = impl->watch_echo;
	memcpy(out->watch_history, impl->watch_history, sizeof(out->watch_history));
	out->watch_history_n = impl->watch_history_n;
	out->watch_bodies = impl->watch_bodies;
	out->watch_clears = impl->watch_clears;
	thread_mutex_unlock(&impl->lock);
}

/* push-0921/B19. Contract in server_remote.h. Main-thread caller (the play
 * driver's per-frame hook), pump-thread writer (remote_record_block_echo) --
 * hence the same lock, which is the one the recorder already writes under. */
void server_remote_play_watch(struct server_remote* remote, bool on, int32_t x,
							  int32_t y, int32_t z) {
	if(!remote || !remote->impl)
		return;
	struct server_remote_impl* impl = remote->impl;
	thread_mutex_lock(&impl->lock);
	/* A MOVE (or a turn-off) drops whatever was stored. The core moves the
	 * watch from the dig cell to the place cell inside one build cycle, so an
	 * echo that outlived its cell would credit the place with the dig's
	 * receipt. RE-STATING THE SAME CELL IS NOT A MOVE: the driver re-states
	 * the watch every frame (mpp_out.watch is published from the FIRST frame
	 * of the wait), and clearing on every re-state would throw away the very
	 * echo it is waiting for -- most likely the one that arrived a
	 * microsecond ago. */
	bool moved = !on || !impl->watch_on || impl->watch_x != x
		|| impl->watch_y != y || impl->watch_z != z;
	if(moved) {
		/* hw1030g diagnostic: count the moves that DROPPED a stored record,
		 * so a fail marker can say whether the echo was cleared by a re-arm
		 * instead of never arriving. */
		if(impl->watch_echo.serial != 0 && impl->watch_clears != UINT32_MAX)
			impl->watch_clears++;
		memset(&impl->watch_echo, 0, sizeof(impl->watch_echo));
		memset(impl->watch_history, 0, sizeof(impl->watch_history));
		impl->watch_history_n = 0;
		impl->watch_bodies = 0;
	}
	impl->watch_on = on;
	if(on) {
		impl->watch_x = x;
		impl->watch_y = y;
		impl->watch_z = z;
	}
	thread_mutex_unlock(&impl->lock);
}

/* M3 readiness census -- contract and rationale in server_remote.h. One
 * bounded pass over the SAME flat entry table server_remote_get_status scans
 * above (linear, 576 entries, the table's only access shape), under the same
 * lock, with no allocation and no writes: this is a pure reader of the
 * ledger, so it cannot perturb promotion order, retention accounting or the
 * epoch pipeline no matter how often the loading screen calls it. */
bool server_remote_ready_census(struct server_remote* remote, int cx, int cz,
								int radius,
								struct server_remote_ready_census* out) {
	if(!out)
		return false;
	memset(out, 0, sizeof(*out));
	if(!remote || !remote->impl)
		return false;
	if(radius < 0)
		radius = 0;
	struct server_remote_impl* impl = remote->impl;
	/* The 9 safe-core columns as a (dz + 1) * 3 + (dx + 1) grid, so each core
	 * coordinate has its own slot: a duplicate entry for one coordinate
	 * (impossible through the table's own create path, but this reader does
	 * not depend on that) can satisfy only that slot, never stand in for a
	 * neighbour the way a bare counter would allow. */
	bool core_seen[9] = {false};
	bool core_res[9] = {false};
	thread_mutex_lock(&impl->lock);
	/* M6 (F5b): THE CLAMP, here rather than at the config read, because this
	 * is where the requested disk meets the radius that will actually be
	 * filled -- and the census is the only reader that holds both. A
	 * ready_radius above admit_radius counted obligations the admission gate
	 * refuses by design, so announced_pending never reached 0 and every join
	 * fell through to the 30 s backstop.
	 *
	 * admit_radius <= 0 means "no admission radius on this connection yet"
	 * (server_remote_start defaults it to SERVER_REMOTE_ADMIT_RADIUS_MAX, so
	 * a live session always has one; a hand-built impl in a test may not) --
	 * clamping to 1 there would silently shrink the disk instead, so that
	 * case is left alone. */
	out->admit_radius = impl->admit_radius;
	if(impl->admit_radius > 0 && radius > impl->admit_radius)
		radius = impl->admit_radius;
	if(radius < 1 && impl->admit_radius > 0)
		radius = 1;
	out->radius_used = radius;
	/* L12: ONE centre. The server's admission centre when it has one, so the
	 * screen is judged against exactly the disk the gate is filling; the
	 * caller's camera column only while no centre is known. */
	int32_t centre_x = 0, centre_z = 0;
	if(remote_admission_centre_locked(impl, &centre_x, &centre_z)) {
		cx = (int)centre_x;
		cz = (int)centre_z;
		out->centre_authoritative = true;
	}
	out->centre_x = cx;
	out->centre_z = cz;
	uint32_t applied = impl->applied_epoch;
	int cold_gate = -1; /* ci-1101/54: -1 = not read yet, 0 refuses, 1 admits */
	for(size_t i = 0; i < REMOTE_CHUNK_ENTRY_CAPACITY; i++) {
		const struct remote_chunk* chunk = &impl->chunks[i];
		if(chunk->state == REMOTE_CHUNK_UNUSED)
			continue;
		/* int64 differences: chunk_x/_z are int32 straight off the wire, so a
		 * hostile or merely absurd coordinate makes `chunk_x - cx` overflow
		 * signed int -- UB, not a large number. The widened subtract and the
		 * widened abs are total over the whole int32 x int32 domain. */
		int64_t dx = (int64_t)chunk->chunk_x - (int64_t)cx;
		int64_t dz = (int64_t)chunk->chunk_z - (int64_t)cz;
		int64_t adx = dx < 0 ? -dx : dx;
		int64_t adz = dz < 0 ? -dz : dz;
		int64_t cheb = adx > adz ? adx : adz;
		if(cheb <= 1) {
			int slot = (int)((dz + 1) * 3 + (dx + 1));
			core_seen[slot] = true;
			if(chunk->state == REMOTE_CHUNK_RESIDENT)
				core_res[slot] = true;
		}
		if(cheb > radius)
			continue;
		if(chunk->state == REMOTE_CHUNK_RESIDENT) {
			out->resident_in_radius++;
			continue;
		}
		/* Astra item 8: a currently announced, in-radius, THIS-epoch entry is
		 * an obligation. TOMBSTONE is deliberately not one -- it is a
		 * coordinate this client can never install (protocol 14 has no chunk
		 * re-request), so counting it would gate the screen forever on
		 * something no pump can drain. Design 2.2 removes tombstoning from
		 * the normal path for exactly that reason. */
		if(chunk->epoch != applied)
			continue;
		/* ci-1101/54: a COLD column the F4 memory gate is refusing right now
		 * is not an obligation this screen can wait out -- the same rule M6
		 * applied to columns past admit_radius. On hw1102b (betamc, dense
		 * spawn) the carve fell to 1440 KB free at 67 of 81 in-radius
		 * columns and stayed there; the 14 left over held the loading
		 * screen for the full 30 s backstop. The gate is read lazily (it
		 * walks the carve free list) and at most once per census. */
		if(!chunk->installed
		   && (chunk->state == REMOTE_CHUNK_ANNOUNCED
			   || chunk->state == REMOTE_CHUNK_RETAINED)) {
			if(cold_gate < 0)
				cold_gate = remote_carve_admits_cold_column() ? 1 : 0;
			if(cold_gate == 0) {
				out->memory_held++;
				continue;
			}
		}
		if(chunk->state == REMOTE_CHUNK_ANNOUNCED
		   || chunk->state == REMOTE_CHUNK_RETAINED
		   || chunk->state == REMOTE_CHUNK_PROMOTING)
			out->announced_pending++;
	}
	thread_mutex_unlock(&impl->lock);
	out->core_resident = true;
	for(int slot = 0; slot < 9; slot++) {
		if(core_seen[slot])
			out->core_announced++;
		if(!core_res[slot])
			out->core_resident = false;
	}
	return true;
}

void server_remote_trace_dump(struct server_remote* remote, FILE* output) {
	if(!remote || !remote->impl || !output)
		return;
	struct server_remote_impl* impl = remote->impl;
	/* REL-MP-FIX3e: copy the ring under frame_lock (which alone guards it),
	 * then format with no lock held -- file I/O never runs under frame_lock. */
	struct remote_trace* copy = malloc(sizeof(impl->traces));
	if(!copy) {
		fprintf(output, "trace dump: %zu-byte snapshot allocation failed\n",
				sizeof(impl->traces));
		return;
	}
	thread_mutex_lock(&impl->frame_lock);
	size_t count = impl->trace_count;
	for(size_t i = 0; i < count; i++)
		copy[i] = impl->traces[(impl->trace_head + i) % REMOTE_TRACE_CAPACITY];
	thread_mutex_unlock(&impl->frame_lock);
	for(size_t i = 0; i < count; i++) {
		const struct remote_trace* trace = &copy[i];
		fprintf(output, "%s gen=%u len=%zu hex=", trace->transmit ? "TX" : "RX",
				trace->generation, trace->length);
		for(size_t j = 0; j < trace->length; j++)
			fprintf(output, "%02X", trace->bytes[j]);
		fputc('\n', output);
	}
	free(copy);
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
void server_remote_test_set_retained_byte_cap(struct server_remote* remote,
											 size_t bytes) {
	assert(remote && remote->impl && bytes > 0);
	struct server_remote_impl* impl = remote->impl;
	thread_mutex_lock(&impl->lock);
	assert(impl->state == REMOTE_DISCONNECTED);
	impl->retained_byte_cap = bytes;
	thread_mutex_unlock(&impl->lock);
}
#endif

#endif /* CAVEWII_MP_CLIENT */
