/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "server_remote_internal.h"

/* W2: the MP backing reservation (mp_backing_configure / _stats) and the
 * `mp.backing_mib` dial it is sized from -- gstate.config_user is the same
 * store main.c reads `mp.install_radius` out of. */
#include "../game/game_state.h"
#include "../platform/mem2.h"

#ifdef CAVEWII_MP_CLIENT

/* Socket lifecycle, the disposable-connector handoff, read/write worker
 * threads, decoded-event/outbound-frame framing + backpressure, and
 * kill/teardown/finalize. Split out of the former monolithic
 * server_remote.c (2026-08-26, maintainer-register item 1); see
 * server_remote_internal.h's top comment for the split's rationale.
 * Functions here that other server_remote_*.c files call are declared
 * non-static in server_remote_internal.h; everything else stays static. */


/* W2 (mp-split-0912, design section 2.4): the compressed-retention budget is
 * no longer a NUMBER this file picks -- it is the MEM2 backing reservation
 * itself (platform/mem2.h, MEM2_MP_BACKING_BYTES: 8 MiB Wii / 12 MiB PC,
 * config `mp.backing_mib` clamped to [2, 16]). impl->retained_byte_cap is
 * filled from mp_backing_stats' CAPACITY, so "bytes held / bytes available"
 * on the status and heartbeat paths describes the real allocator instead of
 * a separate cap that could disagree with it.
 *
 * What this replaces: a 2 MiB Wii / 12 MiB PC constant, explicitly labelled
 * PROVISIONAL ("to be re-derived from a real MEM2-headroom measurement
 * during the S5.4 hardware soak"), tested AFTER the body had already been
 * allocated on the MEM1 heap, and enforced by destroying the farthest
 * column's only authoritative copy. Astra's gate-2 ruling rejected all three
 * halves of that: check capacity BEFORE allocating (item 4), never destroy
 * the only copy (item 5), and do not call an average-derived number a
 * worst-case bound (item 4). The real bound still owes its hardware
 * measurement -- 8 MiB is provisional in the same honest sense -- but it is
 * now a reservation that refuses cleanly rather than a cap that deletes
 * terrain. */

#ifdef PLATFORM_WII
#include <stdio.h>
// S5.3 hunt PROBE (temporary, round 2): one-shot opcode log for the first 3
// events the read thread's decoder produces -- discriminates decode-side
// progress (does the handshake reply actually get decoded into an
// MP_EVENT_HANDSHAKE?) from dispatch-side death (main-thread process_event /
// the condvar handoff to the write thread), which round 1's evidence could
// not separate. Read-thread-only writes.
static void cw_decode_trace(int event_type) {
	static unsigned budget = 6;
	if(budget == 0 || CW_SD_TRACE_OFF())
		return;
	budget--;
	// ROUND 8: cw_mp_trace.txt -> cw_trace_read.txt (own file per OS thread;
	// see net_socket.c's net_socket_connect/_receive/_send comments for the
	// full inventory of who used to share this filename).
	FILE* f = fopen("sd:/cw_trace_read.txt", "a");
	if(!f)
		return;
	fprintf(f, "decode_event type=%d\n", event_type);
	fflush(f);
	fclose(f);
}
// S5.3 hunt PROBE (temporary, round 2 cont.): brackets remote->lock
// acquire/release around the read thread's per-packet locked region --
// take 9 showed the read thread decode a 3rd event (MP_EVENT_SPAWN_POSITION)
// that process_event NEVER got to dispatch, while every other probe (main
// loop, condvar) went silent at the same moment. Refined hypothesis: the
// read thread stalls/faults INSIDE this locked region for that packet,
// leaving remote->lock held forever -- which would explain the main
// thread's OWN death as a downstream block on server_remote_pump's first
// lock (see cw_pump_lock_trace in server_remote.c), not a main-loop crash
// in its own right. Read-thread-only writes, own small budget.
static void cw_lock_trace(const char* label, int event_type) {
	static unsigned budget = 12;
	if(budget == 0 || CW_SD_TRACE_OFF())
		return;
	budget--;
	FILE* f = fopen("sd:/cw_trace_read.txt", "a"); // ROUND 8: own file, see cw_decode_trace
	if(!f)
		return;
	fprintf(f, "%s type=%d\n", label, event_type);
	fflush(f);
	fclose(f);
}
// S5.3 hunt PROBE (temporary, ROUND 6): scans impl->read_scratch_canary
// (server_remote_internal.h field comment) from the read thread's own
// marker sites -- read-thread-only writes, own small budget, same
// cw_mp_trace.txt every other read-thread tracer here already uses.
static void cw_read_canary_scan(struct server_remote_impl* remote,
								const char* label) {
	static unsigned budget = 20;
	char tag[64];
	snprintf(tag, sizeof(tag), "read_scratch@%s", label ? label : "?");
	cw_canary_scan("sd:/cw_trace_read.txt", tag, &remote->read_scratch_canary,
				   &budget); // ROUND 8: own file, see cw_decode_trace
}
// S5.3 hunt PROBE (ROUND 6 introduced, ROUND 7 REMOVED): a per-LWP stack
// watermark pair used to live here, painted at thread entry from
// __builtin_frame_address(0). Codex's round-7 static review caught the
// defect: at thread ENTRY, that builtin reads the frame address AFTER the
// function's own prologue already dropped its frame (remote_read_thread's
// own -fstack-usage frame is ~10,000 bytes -- see build-dev's .su), so
// "entry_sp - 64KB + 4KB" landed the paint FLOOR ~5.9KB below the actual
// 64KB LWP stack allocation -- INTO HEAP METADATA, a real scribbler that ran
// in takes 22/24/25 and may have caused those takes' crash sites (round-6's
// LWP-watermark readings and the crash locations they were taken alongside
// are TAINTED; the main-stack watermark against __ppc_main_sp -- which does
// NOT have this bug, see below -- is unaffected and remains the load-bearing
// instrument). The write thread's identical code only escaped by luck (its
// own frame is a mere ~1,080 bytes, so the same miscomputation still landed
// inside real stack). Fix: dropped entirely rather than patched -- less rig
// to trust, and the main-stack watermark plus the struct guard canaries
// (gstate/clin_channel/read_scratch, all heap/BSS-based and untouched by
// this bug class) already cover this round's corruption-detection goal.
// S5.3 hunt PROBE (temporary, ROUND 5/take 20): take 20's evidence narrowed
// the death to a ~1-line window deep in the MAIN thread's tick loop, several
// frames after server_remote_pump() had already returned cleanly -- ruling
// out a live main-thread stall AT that call, but NOT a delayed corruption
// left behind by it. The MP_EVENT_HANDSHAKE case (server_remote_protocol.c)
// is the one that actually ran that frame: it builds a login mp_frame on
// process_event's stack and hands it to queue_frame(), which
// thread_cond_signal()s this write thread awake to perform the FIRST real
// queued-frame socket send since the provider swap (the handshake's own
// 17-byte send went out through a separate direct/blocking path during
// connector setup, never through this cond-signalled queue). Every prior S5.3
// root cause has been "first time this exact cross-thread path fires on real
// hardware" (S5.1a's LWP cond, net_socket's connect retry, the SIGPIPE fix,
// the item_map stack frame) -- this brackets the SAME class of suspect: does
// the write thread wake, pop, and send this first queued frame cleanly?
// Separate file (own single writer -- server_remote_transport.c's read-
// thread tracers above are documented connector/read-thread-only; cw_mp_trace
// is NOT reused here to keep that single-writer-per-file discipline intact
// for a second, genuinely concurrent thread), own small budget, unconditional
// (no hw_run gate, matching cw_dispatch_trace_bare's precedent).
static void cw_write_trace(const char* label, uint32_t detail_a,
						   uint32_t detail_b) {
	static unsigned budget = 40;
	if(budget == 0 || CW_SD_TRACE_OFF())
		return;
	budget--;
	// ROUND 8: renamed cw_wt_trace.txt -> cw_trace_write.txt so it lines up
	// with net_socket_send's first_send log (net_socket.c), which turned out
	// to be misrouted to the read/connector threads' file -- both now share
	// THIS file instead, correctly (same write thread, single writer).
	FILE* f = fopen("sd:/cw_trace_write.txt", "a");
	if(!f)
		return;
	fprintf(f, "%s a=%lu b=%lu\n", label, (unsigned long)detail_a,
			(unsigned long)detail_b);
	fflush(f);
	fclose(f);
}
#endif

void set_reason_locked(struct server_remote_impl* remote,
								 const char* reason) {
	snprintf(remote->reason, sizeof(remote->reason), "%s",
			 reason ? reason : "remote transport error");
}

/* W6 (mp-split-0912/35): FIRST-WINS reason latch, the text half of the
 * mp_end trio.
 *
 * Why a second setter exists. server_remote_status::reason -- the string the
 * disconnect screen renders and the string main.c copies into the heartbeat's
 * mp_end_reason -- is a straight copy of impl->reason (server_remote.c:580),
 * and server_remote_start clears it, so "empty" means "nothing has explained
 * THIS connect generation yet". The read loop's EOF/error exit used to latch
 * mp_end/mp_end_phase/mp_end_rx directly but leave the TEXT to the pump: it
 * called publish_error and relied on server_remote_handle_event's
 * REMOTE_EVENT_ERROR branch to run set_reason_locked. Two ways that loses the
 * text outright, both observed:
 *   - the exit takes the `expected` branch (stop_now / graceful_stop), which
 *     publishes nothing at all -- real-Wii run betamc_walk_0913 tick 321
 *     reported mp_end=2, mp_end_phase=6 (REMOTE_CLOSING, i.e. a
 *     server_remote_disconnect("Quitting") whose peer then FIN'd) and
 *     mp_end_reason="";
 *   - the error IS published but the pump never drains it before teardown, or
 *     publish_error_locked's own stop guard refuses it because another thread
 *     set stop_now in the window where this branch has released the lock.
 * Either way an unattended run reports a terminal cause with no cause.
 *
 * So the terminal text is now latched in the SAME critical section as the
 * mp_end trio, and first-wins for the same reason the trio is: the first
 * explanation is the diagnosis, everything after it is consequence. That is
 * what keeps a Packet255 kick text, or an earlier chunk/codec diagnostic,
 * from being overwritten by the close that follows it -- the guard
 * mp-split-0912/5 added in the pump, applied at the source. */
static void latch_reason_locked(struct server_remote_impl* remote,
								const char* reason) {
	if(remote->reason[0] == '\0')
		set_reason_locked(remote, reason);
}

/* Protocol-phase word for the terminal diagnostics. B1 named only the three
 * phases a connect can die IN (play / login / handshake) and folded every
 * other state into "handshake"; the betamc_walk_0913 evidence landed on
 * REMOTE_CLOSING (mp_end_phase=6), which that fold would have reported as a
 * handshake failure -- a wrong diagnosis, not just a vague one. The three
 * original words keep their exact spelling: test_mp_s0_env's EOF probes and
 * the ordering table match on them. */
static const char* remote_phase_name(enum server_remote_state state) {
	switch(state) {
		case REMOTE_PLAY: return "play";
		case REMOTE_LOGIN_SENT: return "login";
		case REMOTE_HANDSHAKE_SENT: return "handshake";
		case REMOTE_CONNECTING: return "connecting";
		case REMOTE_KICKED: return "kicked";
		case REMOTE_CLOSING: return "closing";
		case REMOTE_DISCONNECTED: return "disconnected";
	}
	return "handshake";
}

/* --- Socket teardown: shut down NOW, close only AFTER the workers join ---
 *
 * INVARIANT (fd-reuse safety; 2026-09-01 audit item 3, transport.c:618/623/162):
 *   net_socket_close(fd) -- the ONLY step that releases the fd NUMBER back to
 *   the OS for reuse -- runs exclusively after join_workers() has returned,
 *   i.e. only once BOTH worker threads have provably stopped calling
 *   recv()/send(). No other call site may close remote->socket.
 *
 * Why. remote_read_thread caches the fd in its thread-local `connected` and
 * remote_write_thread in its thread-local `socket`, then calls
 * net_socket_receive/net_socket_send OUTSIDE impl->lock. A closer that does
 * shutdown AND close together under the lock therefore frees a number a peer
 * thread still holds. The read loop's pre-recv stop_now check narrows the
 * window but cannot close it: it reads stop_now under the lock, UNLOCKS, and
 * only then calls recv(). A closer running in that gap (a kill site on the
 * pump/caller thread in server_remote_protocol.c, the write thread's
 * send-failure branch, the read thread's own graceful exit) releases the
 * number, the OS may immediately hand it to any other socket in the process,
 * and the stale thread then recv()s a FOREIGN stream on a perfectly valid
 * fd -- no error, silent cross-talk.
 *
 * net_socket_shutdown does NOT free the number; it only makes the endpoint
 * non-functional (and sends FIN), which is what actually unblocks an
 * in-flight recv() with count <= 0 and wakes the read loop. So the split is:
 * shutdown + publish `socket = NET_SOCKET_INVALID` immediately under the lock
 * -- every lock-guarded reader (queue_frame_locked, send_keep_alive,
 * get_status's socket_open, the read loop's own check) still sees the
 * connection as gone at exactly the same moment as before -- park the raw
 * number in pending_close_socket, and release it in
 * close_pending_socket_after_join(), which is only ever called after
 * join_workers().
 *
 * server_remote_cancel already implemented this split INLINE (Fix A part 2,
 * docs/mp_s1_hostile_review_2026-08-20.md:30); it was the only correct close
 * path in the file. These two helpers hoist that one implementation so every
 * closer shares it instead of duplicating half of it.
 *
 * At most one fd per session reaches the parked slot: impl->socket is
 * assigned exactly once per connection (remote_read_thread, right after the
 * connector handoff), and close_socket_locked early-returns once it is
 * INVALID. server_remote_start refuses unless state == REMOTE_DISCONNECTED,
 * which only server_remote_cancel sets -- and cancel drains the slot -- so a
 * new connection can never begin with an unclosed predecessor parked.
 *
 * WHEN the parked fd is released -- and why NOT simply "at cancel". Parking
 * it all the way to cancel is safe but it is NOT behaviour-neutral, and the
 * loopback suite proved it: a kill leaves the peer still sending, and it was
 * the CLOSE (with unread bytes queued) that produced the RST which made that
 * peer's next send() fail. Under a shutdown-only teardown Linux keeps
 * ACKing and buffering into the shut-down receive queue instead of
 * resetting, so a server that keeps streaming just fills the window and
 * blocks -- test_event_queue_overflow_kill_reports' flood peer deadlocked in
 * send() for exactly this reason (it passed on Windows only on buffering
 * luck). Holding the fd to cancel would therefore silently stop a killed
 * client from shoving back on a still-sending server.
 *
 * So the release point is the EARLIEST moment the invariant permits, which
 * is also the moment the old code released it: as soon as BOTH workers are
 * provably past their last socket call. Each worker sets its own
 * read_thread_exited/write_thread_exited UNDER THIS LOCK, after its final
 * recv()/send() and with no socket call after it, so a closer that observes
 * both flags under the lock has the same guarantee join gives -- neither
 * thread can issue another recv()/send() on this number -- without waiting
 * for the OS thread to be reaped. release_pending_socket_locked() is called
 * from close_socket_locked itself and from each worker's exit block, so the
 * fd goes back to the OS within one wakeup of the kill, exactly as before.
 * join_workers + close_pending_socket_after_join in server_remote_cancel
 * remain the backstop for a session torn down while a worker is still live
 * (cancel-during-connect, a worker wedged in a blocking call).
 *
 * Cost: nothing measurable. In the worst case the number is released one
 * worker wakeup later than the old immediate close -- and the read thread's
 * ordinary "peer closed" exit (count <= 0, no graceful_stop) already held
 * the fd until cancel before this change. */

/* True once NEITHER worker can touch the socket again: each *_exited flag is
 * set under this lock after that thread's last recv()/send(), and
 * join_workers clears *_started, so a joined worker satisfies it too. */
static bool workers_past_socket_locked(const struct server_remote_impl* remote) {
	return (!remote->read_thread_started || remote->read_thread_exited)
		&& (!remote->write_thread_started || remote->write_thread_exited);
}

/* Release a parked fd if -- and only if -- it is provably unreachable. Safe
 * to call from anywhere under the lock; it is a no-op while either worker
 * could still be mid-recv()/mid-send(). */
static void release_pending_socket_locked(struct server_remote_impl* remote) {
	if(remote->pending_close_socket == NET_SOCKET_INVALID)
		return;
	if(!workers_past_socket_locked(remote))
		return;
	/* THE invariant, checked at the one place that frees the number. Every
	 * session teardown in the suite runs through here, so an edit that ever
	 * reintroduces an early close trips this rather than shipping a silent
	 * fd-reuse race. */
	assert(workers_past_socket_locked(remote));
	net_socket_close(remote->pending_close_socket);
	remote->pending_close_socket = NET_SOCKET_INVALID;
}

void close_socket_locked(struct server_remote_impl* remote) {
	/* REL-MP-FIX3e: every stop_now = true site calls this, so it is where the
	 * writer (which waits under frame_lock only) learns of the stop. */
	/* REL-MP-FIX5: and where a reader parked on a full event ring learns of
	 * it (caller holds `lock`, the condvar's mutex). */
	thread_cond_broadcast(&remote->event_space);
	thread_mutex_lock(&remote->frame_lock);
	remote->w_socket = NET_SOCKET_INVALID;
	remote->w_stop = remote->w_stop || remote->stop_now;
	thread_cond_broadcast(&remote->write_signal);
	thread_mutex_unlock(&remote->frame_lock);
	if(remote->socket == NET_SOCKET_INVALID)
		return;
	net_socket_shutdown(remote->socket);
	/* One fd per session, by the argument above; parking a second would
	 * silently leak the first. */
	assert(remote->pending_close_socket == NET_SOCKET_INVALID);
	remote->pending_close_socket = remote->socket;
	remote->socket = NET_SOCKET_INVALID;
	/* A closer that runs after both workers are already done (teardown from
	 * the caller thread, or the second worker's own exit block) releases the
	 * number immediately -- same instant as the old shutdown+close. */
	release_pending_socket_locked(remote);
}

/* Backstop release for the teardown paths that JOIN the workers rather than
 * wait for them to flag themselves exited (server_remote_cancel,
 * server_remote_start's write-thread-creation failure). join_workers clears
 * read/write_thread_started, so the predicate above is satisfied by
 * construction here. Call only AFTER the join. */
static void close_pending_socket_after_join(struct server_remote_impl* remote) {
	thread_mutex_lock(&remote->lock);
	/* The parked fd is by construction the one close_socket_locked took out
	 * of `socket`, never a live one. */
	assert(remote->socket == NET_SOCKET_INVALID
		   || remote->pending_close_socket == NET_SOCKET_INVALID);
	release_pending_socket_locked(remote);
	assert(remote->pending_close_socket == NET_SOCKET_INVALID);
	thread_mutex_unlock(&remote->lock);
}

/* Caller holds frame_lock (REL-MP-FIX3e). */
static void trace_locked(struct server_remote_impl* remote, uint32_t generation,
						 bool transmit, const uint8_t* bytes, size_t length) {
	if(length > MP_S0_FRAME_CAPACITY)
		return;
	if(remote->trace_count == REMOTE_TRACE_CAPACITY) {
		remote->trace_head = (remote->trace_head + 1) % REMOTE_TRACE_CAPACITY;
		remote->trace_count--;
	}
	size_t index
		= (remote->trace_head + remote->trace_count) % REMOTE_TRACE_CAPACITY;
	struct remote_trace* trace = &remote->traces[index];
	trace->transmit = transmit;
	trace->generation = generation;
	trace->length = length;
	memcpy(trace->bytes, bytes, length);
	remote->trace_count++;
}

/* Root cause (2026-08-19 live-server investigation, task priority fix):
 * push_event_locked used to treat a MOMENTARILY full 64-slot event queue as
 * "unretainable" and instantly self-disconnect (stop_now + an abortive
 * close_socket_locked). The read thread decodes as fast as the OS delivers
 * bytes, with NO throttle, while the queue's only consumer
 * (server_remote_pump) runs once per caller frame -- so a real, populated
 * server's ordinary post-login burst (one MP_EVENT_MOB_SPAWN per nearby
 * entity, ~20-25 bytes each, dozens arriving in a couple of recv() calls)
 * routinely produced more than 64 queued events before the very next pump()
 * call, well before any hostile/pathological flood. Every synthetic test
 * peer (peer_entry/fault_peer_entry) sends at most a handful of packets, so
 * this path was never exercised until a real mc173-server was used -- hence
 * "loopback fixtures pass, live dies ~instantly-to-a-few-hundred-ms after
 * accept" with no fixed timeout anywhere in this file. Closing a socket
 * that still has unread bytes queued in the OS receive buffer is an
 * ABORTIVE close (RST) on Windows, which is exactly the "client side
 * closed" / os error 10053 the peer observed -- self-inflicted, not a
 * server or protocol problem.
 *
 * Fix: apply backpressure instead of instant abort. These events
 * (unlike Packet51 chunk bodies, which the registry provides no re-request
 * for and so keep their own bounded RETAIN/evict/tombstone store) have no
 * re-request problem -- stalling the read thread while server_remote_pump
 * catches up loses nothing.
 *
 * REL-MP-FIX5 (real Wii hw1021c, "remote event queue overflow (pump idle
 * 557ms)" ~3 s after login): the old 500 ms patience killed sessions on
 * ordinary slow post-boot frames (557-984 ms). Beta's client receive queue
 * is unbounded and drained per tick on the main thread with no
 * receive-overflow kill (NetworkManager.java:24 readPackets, :137 add,
 * :187-208 processReadPackets); its only receive timeout is 1200 ticks with
 * nothing read (:193). So a full ring now parks the reader on event_space
 * with no short deadline: the read thread stops calling recv(), the kernel
 * receive window closes and TCP flow control back-pressures the server
 * (bounded by the server's own Beta rules). The decoder keeps its partial
 * bytes untouched. Wakers, all under `lock`: the pump after each pop
 * (progress), close_socket_locked (every stop_now site: cancel, kill,
 * kick), drain_event_queue_locked and each generation bump; each changes
 * the predicate and signals under `lock`, so no wakeup can be lost. The
 * REMOTE_EVENT_WAIT_SLICE_MS slice is only a defensive re-check. Lock order: event_space is waited on
 * under `lock` only, never with frame_lock held, so FIX3e's order (`lock`
 * may take frame_lock, never the reverse) is untouched. The one backstop is
 * Beta's own 1200-tick (60 s) silence, reported honestly as a main-thread
 * stall. */
#define REMOTE_EVENT_STALL_BACKSTOP_MS 60000u /* NetworkManager.java:193 */
#define REMOTE_EVENT_WAIT_SLICE_MS 100u

static bool push_event_locked(struct server_remote_impl* remote,
								 const struct remote_event* event) {
	if(event->generation != remote->generation || remote->stop_now)
		return false;
	if(remote->event_count == REMOTE_QUEUE_CAPACITY)
		remote->event_backpressure_waits++;
	/* Only the pump frees slots and only this thread fills them, so staying
	 * in this loop means the pump made no progress since wait_start. */
	ptime_t wait_start = time_get();
	while(remote->event_count == REMOTE_QUEUE_CAPACITY) {
		int32_t stalled_ms = time_diff_ms(wait_start, time_get());
		if(stalled_ms >= (int32_t)REMOTE_EVENT_STALL_BACKSTOP_MS) {
			char reason[96];
			snprintf(reason, sizeof(reason),
					 "main thread stalled %ldms (remote event queue full)",
					 (long)stalled_ms);
			kill_connection_locked(remote, reason);
			return false;
		}
		(void)thread_cond_timedwait(&remote->event_space, &remote->lock,
									REMOTE_EVENT_WAIT_SLICE_MS);
		/* Re-check after reacquiring: cancel/reconnect may have run while
		 * this thread was parked. */
		if(remote->stop_now || event->generation != remote->generation)
			return false;
	}
	size_t index
		= (remote->event_head + remote->event_count) % REMOTE_QUEUE_CAPACITY;
	remote->events[index] = *event;
	remote->event_count++;
	if(remote->event_count > remote->queued_events_peak)
		remote->queued_events_peak = remote->event_count;
	return true;
}

bool pop_event_locked(struct server_remote_impl* remote,
								struct remote_event* event) {
	if(remote->event_count == 0)
		return false;
	*event = remote->events[remote->event_head];
	remote->event_head = (remote->event_head + 1) % REMOTE_QUEUE_CAPACITY;
	remote->event_count--;
	return true;
}

bool queue_frame_locked(struct server_remote_impl* remote,
						 const struct mp_frame* frame, bool correction_echo) {
	if(remote->stop_now || remote->graceful_stop)
		return false;
	thread_mutex_lock(&remote->frame_lock);
	bool full = remote->frame_count == REMOTE_QUEUE_CAPACITY;
	if(!full) {
		size_t index = (remote->frame_head + remote->frame_count)
			% REMOTE_QUEUE_CAPACITY;
		remote->frames[index].generation = remote->generation;
		remote->frames[index].frame = *frame;
		remote->frames[index].correction_echo = correction_echo;
		remote->frame_count++;
		thread_cond_signal(&remote->write_signal);
	}
	thread_mutex_unlock(&remote->frame_lock);
	/* The kill takes frame_lock itself (close_socket_locked). */
	if(full)
		kill_connection_locked(remote, "remote control queue overflow");
	return !full;
}

bool queue_frame(struct server_remote_impl* remote,
						const struct mp_frame* frame) {
	thread_mutex_lock(&remote->lock);
	bool result = queue_frame_locked(remote, frame, false);
	thread_mutex_unlock(&remote->lock);
	return result;
}

/* Fix B (see server_remote_internal.h's declaration comment): identical to
 * queue_frame, plus a REMOTE_PLAY re-check under the SAME lock acquisition
 * as the append. Used by remote_process_srpc's SRPC cases, whose own
 * REMOTE_PLAY gate check (server_remote_protocol.c) runs under a separate,
 * earlier lock acquisition and can no longer be trusted alone. */
bool queue_frame_playing(struct server_remote_impl* remote,
						  const struct mp_frame* frame) {
	thread_mutex_lock(&remote->lock);
	bool queued = remote->state == REMOTE_PLAY
		&& queue_frame_locked(remote, frame, false);
	thread_mutex_unlock(&remote->lock);
	return queued;
}

static bool encode_movement_frame(const struct mp_movement* movement,
								   enum mp_event_type type,
								   struct mp_frame* frame) {
	switch(type) {
		case MP_EVENT_FLYING:
			return mp_encode_flying(frame, movement->on_ground);
		case MP_EVENT_PLAYER_POSITION:
			return mp_encode_player_position(
				frame, movement->x, movement->y, movement->stance,
				movement->z, movement->on_ground);
		case MP_EVENT_PLAYER_LOOK:
			return mp_encode_player_look(
				frame, movement->yaw, movement->pitch, movement->on_ground);
		case MP_EVENT_PLAYER_LOOK_MOVE:
			return mp_encode_player_look_move(
				frame, movement->x, movement->y, movement->stance,
				movement->z, movement->yaw, movement->pitch,
				movement->on_ground);
		default:
			return false;
	}
}

bool queue_movement(struct server_remote_impl* remote,
						   const struct mp_movement* movement,
						   enum mp_event_type type, bool correction_echo) {
	struct mp_frame frame;
	if(!encode_movement_frame(movement, type, &frame))
		return false;
	thread_mutex_lock(&remote->lock);
	bool queued = queue_frame_locked(remote, &frame, correction_echo);
	thread_mutex_unlock(&remote->lock);
	return queued;
}

/* Fix B: queue_frame_playing's counterpart for remote_process_srpc's
 * SRPC_PLAYER_POS case, the only queue_movement caller reached through the
 * same TOCTOU-prone REMOTE_PLAY gate (remote_handle_correction's own
 * queue_movement echo runs from inside process_event's dispatch, which only
 * ever runs while REMOTE_PLAY was already the state that produced the
 * correction event -- not gated by a separately-timed check). */
bool queue_movement_playing(struct server_remote_impl* remote,
							 const struct mp_movement* movement,
							 enum mp_event_type type) {
	struct mp_frame frame;
	if(!encode_movement_frame(movement, type, &frame))
		return false;
	thread_mutex_lock(&remote->lock);
	bool queued = remote->state == REMOTE_PLAY
		&& queue_frame_locked(remote, &frame, false);
	thread_mutex_unlock(&remote->lock);
	return queued;
}

/* Set the error side-slot; see its declaration comment. Safe from any
 * thread, never waits, never touches the event ring. First error wins --
 * later ones are dropped (the first is the diagnostic; everything after a
 * terminal error is consequence). */
static void publish_error_locked(struct server_remote_impl* remote,
								 uint32_t generation, const char* message) {
	if(remote->error_pending || generation != remote->generation
	   || remote->stop_now)
		return;
	remote->error_pending = true;
	remote->error_pending_generation = generation;
	snprintf(remote->error_pending_msg, sizeof(remote->error_pending_msg),
			 "%s", message);
}

void publish_error(struct server_remote_impl* remote,
						  uint32_t generation, const char* message) {
	thread_mutex_lock(&remote->lock);
	publish_error_locked(remote, generation, message);
	thread_mutex_unlock(&remote->lock);
}

/* Terminal teardown from ANY thread, under the lock (re-review finding 1):
 * reason + a DELIVERED error (side-slot, published BEFORE stop_now so
 * publish_error_locked's stop guard cannot refuse it) + stop + socket
 * close + writer wake. Every kill site MUST use this instead of
 * hand-rolling the sequence: a kill that sets stop_now without delivering
 * an error leaves state at REMOTE_PLAY forever -- the pump never learns,
 * the seam's B5 deadline re-arms every REMOTE_PLAY frame, and the frozen
 * session can never be torn down (the original finding-5 wedge, restored
 * through the back door). */
void kill_connection_locked(struct server_remote_impl* remote,
								   const char* reason) {
	set_reason_locked(remote, reason);
	publish_error_locked(remote, remote->generation, reason);
	remote->stop_now = true;
	close_socket_locked(remote);
	thread_cond_broadcast(&remote->write_signal);
}

/* F5a (review finding H2): the decoder's body-allocation reclaim seam.
 * Called from inside mp_decoder_feed on the READ thread with the remote lock
 * NOT held (see the read loop), so it takes the lock the reclaim needs. A
 * false return means the ladder has already ended the session with the
 * capacity reason -- the decoder then stops trying and the read loop's
 * decode-error terminal reports capacity, not a protocol error. */
static bool remote_decoder_reclaim_cb(void* ctx, size_t wanted) {
	struct server_remote_impl* remote = (struct server_remote_impl*)ctx;
	if(!remote)
		return false;
	thread_mutex_lock(&remote->lock);
	bool reclaimed = remote_backing_reclaim_or_fail_locked(remote, wanted);
	if(!reclaimed)
		remote_backing_fail_guard_locked(remote);
	thread_mutex_unlock(&remote->lock);
	return reclaimed;
}

void remote_attach_decoder_reclaim(struct server_remote_impl* remote,
								   struct mp_decoder* decoder) {
	mp_decoder_set_reclaim_hook(decoder, remote_decoder_reclaim_cb, remote);
}

bool remote_read_decode_error(struct server_remote_impl* remote,
							  const struct mp_decoder* decoder,
							  uint32_t generation) {
	thread_mutex_lock(&remote->lock);
	bool capacity = false;
	if(decoder->allocation_failed) {
		remote->retention_allocation_failures++;
		/* W2 (design section 2.4): a Packet51 body refused by the backing
		 * reservation is a CAPACITY failure, not a protocol error -- "bad or
		 * unsupported P0 packet" would tell the player their server is broken
		 * when the truth is that its world does not fit this console. Only
		 * the MAP_CHUNK packet id qualifies: the multi-block and explosion
		 * allocations are ordinary heap and their failure genuinely is
		 * generic pressure.
		 *
		 * F5a (review finding H2): UNCONDITIONAL now. W2 ran the reclaim
		 * ladder from right here and treated only its FAILURE as capacity --
		 * so a reclaim that succeeded (after decoder_error had already thrown
		 * the body away, making the freed bytes useless) fell through to the
		 * "bad or unsupported P0 packet" publish with mp_end never latched:
		 * the player was told their server was broken and an unattended Wii
		 * run recorded mp_end=0 for a capacity death. The reclaim-and-RETRY
		 * now happens inside the decoder, before the body is discarded
		 * (mp_codec.c's Packet51 header case, wired by
		 * remote_attach_decoder_reclaim); by the time the decode error
		 * reaches here the retry is already spent, so this is the terminal
		 * and nothing else. remote_backing_capacity_fail_locked is idempotent
		 * on a session the hook's own ladder already killed -- it keeps the
		 * first reason and the first mp_end. */
		if(decoder->error_packet_id == MP_PACKET_MAP_CHUNK) {
			remote_backing_capacity_fail_locked(remote,
												decoder->alloc_wanted);
			capacity = true;
		}
	}
	thread_mutex_unlock(&remote->lock);
	if(capacity)
		/* kill_connection_locked already published the reason, closed the
		 * socket and set stop_now. */
		return true;
	char decode_reason[96];
	snprintf(decode_reason, sizeof(decode_reason),
			 "bad or unsupported P0 packet id=0x%02X phase=%u",
			 decoder->error_packet_id, decoder->error_phase);
	publish_error(remote, generation, decode_reason);
	return false;
}

/* Disposable-connector handoff (2026-08-20 provider-swap redesign, hostile-
 * review finding 2). net_socket_connect can block unboundedly in
 * getaddrinfo (DNS has no portable cancel) and ~10s in the bounded TCP
 * phase; running it ON the read thread made server_remote_cancel's
 * thread_join a main-thread hang for that whole window. The connect now
 * runs on a DETACHED throwaway thread whose only shared state is this
 * self-contained refcounted block -- it never touches the impl, so cancel
 * simply marks it `abandoned` and walks away (no join, no use-after-free
 * window); the connector closes any socket it produced, drops the last
 * ref, frees the block, and dies whenever the OS releases it. */
struct remote_connect_handoff {
	struct thread_mutex lock;
	int refs; /* 2: the read thread + the connector */
	bool abandoned;
	bool done;
	net_socket_t socket;
	char error[256];
	char host[256];
	uint16_t port;
};

/* Drop one ref under the handoff lock; destroys the block on the last. */
static void handoff_unref(struct remote_connect_handoff* handoff) {
	thread_mutex_lock(&handoff->lock);
	bool last = --handoff->refs == 0;
	thread_mutex_unlock(&handoff->lock);
	if(last) {
		thread_mutex_destroy(&handoff->lock);
		free(handoff);
	}
}

static void* remote_connector_thread(void* user) {
	struct remote_connect_handoff* handoff = user;
	char error[256] = {0};
	/* Pin the process's Winsock session for this thread's whole lifetime
	 * (re-review finding 4): an ABANDONED connector can outlive
	 * server_remote_destroy's net_socket_platform_cleanup, and
	 * WSAStartup/WSACleanup are refcounted -- this ref keeps the session
	 * alive until the connector's own cleanup below. POSIX: both are
	 * no-ops. */
	char wsa_error[64];
	bool session_pinned = net_socket_platform_init(wsa_error,
												   sizeof(wsa_error));
	net_socket_t connected = !session_pinned
		? NET_SOCKET_INVALID
		: net_socket_connect(handoff->host, handoff->port, error,
							 sizeof(error));
	if(!session_pinned)
		snprintf(error, sizeof(error), "socket session init failed");
	thread_mutex_lock(&handoff->lock);
	if(handoff->abandoned) {
		if(connected != NET_SOCKET_INVALID)
			net_socket_close(connected);
	} else {
		handoff->socket = connected;
		snprintf(handoff->error, sizeof(handoff->error), "%s", error);
		handoff->done = true;
	}
	thread_mutex_unlock(&handoff->lock);
	handoff_unref(handoff);
	if(session_pinned)
		net_socket_platform_cleanup();
	return NULL;
}

static void* remote_read_thread(void* user) {
	// ROUND 6's LWP stack-floor paint (from __builtin_frame_address(0) at
	// entry) was REMOVED in round 7 -- Codex's static review proved it wrote
	// ~5.9KB below this thread's actual 64KB stack allocation, into heap
	// metadata (see the removed block's replacement comment near
	// cw_read_canary_scan above for the full mechanism).
	struct server_remote_impl* remote = user;
	thread_mutex_lock(&remote->lock);
	uint32_t generation = remote->generation;
	char error[256] = {0};
	struct remote_connect_handoff* handoff
		= calloc(1, sizeof(struct remote_connect_handoff));
	net_socket_t connected = NET_SOCKET_INVALID;
	if(handoff) {
		snprintf(handoff->host, sizeof(handoff->host), "%s", remote->host);
		handoff->port = remote->port;
		handoff->socket = NET_SOCKET_INVALID;
		handoff->refs = 2;
	}
	thread_mutex_unlock(&remote->lock);

	bool connector_running = false;
	/* S5: spawn/dispose through the platform seam -- PC detaches (OS reaps),
	 * Wii keeps the handle so a COMPLETED connector's 64 KB stack is
	 * reclaimed on join, while an ABANDONED one is leaked on purpose (the
	 * documented disposable-connector contract, thread.h). Priority 32 =
	 * the mesher band: connect-phase work never competes with a live
	 * connection's read/write pair. */
	struct thread connector;
	if(handoff && thread_mutex_init(&handoff->lock)) {
		if(thread_spawn_disposable(&connector, remote_connector_thread,
								   handoff, 32)) {
			connector_running = true;
		} else {
			thread_mutex_destroy(&handoff->lock);
		}
	}
	if(!connector_running) {
		if(handoff)
			free(handoff);
		handoff = NULL;
		snprintf(error, sizeof(error), "connect thread creation failed");
	}

	/* Poll the handoff alongside stop/generation: cancel during the connect
	 * phase now exits this thread within ~10ms instead of blocking in the
	 * OS resolver/handshake (the old finding-2 hang). */
	while(connector_running) {
		thread_mutex_lock(&remote->lock);
		bool stop = remote->stop_now || generation != remote->generation;
		thread_mutex_unlock(&remote->lock);
		thread_mutex_lock(&handoff->lock);
		bool done = handoff->done;
		if(done) {
			connected = handoff->socket;
			snprintf(error, sizeof(error), "%s", handoff->error);
		} else if(stop) {
			handoff->abandoned = true;
		}
		thread_mutex_unlock(&handoff->lock);
		if(done || stop) {
			handoff_unref(handoff);
			handoff = NULL;
			/* done => the connector finished (its risky blocking phase is
			 * over; join returns promptly). stop-without-done => abandoned
			 * mid-connect: never join, walk away. */
			thread_dispose(&connector, done);
			break;
		}
		thread_msleep(10);
	}
	thread_mutex_lock(&remote->lock);
	if(connected == NET_SOCKET_INVALID || remote->stop_now
	   || generation != remote->generation) {
		if(connected != NET_SOCKET_INVALID)
			net_socket_close(connected);
		if(!remote->stop_now && generation == remote->generation)
			publish_error_locked(remote, generation,
								 error[0] ? error : "connect cancelled");
		/* Terminal latch, connect leg (mp-split-0912/5): no socket ever
		 * opened, so rx is 0 by construction and the phase is CONNECTING. */
		if(remote->mp_end == 0 && generation == remote->generation) {
			remote->mp_end = 4u;
			remote->mp_end_phase = (uint32_t)REMOTE_CONNECTING;
			remote->mp_end_rx = 0;
			/* W6: and the text, under the SAME generation guard -- writing
			 * reason for a stale generation would put this dead connect's
			 * complaint on a live newer session. The publish above is
			 * skipped entirely when stop_now is set (a cancel landing during
			 * connect), which is the mp_end=4 twin of the EOF hole: a
			 * terminal cause with no cause. */
			latch_reason_locked(remote,
								error[0] ? error
								: remote->stop_now
									? "connect cancelled locally, connecting"
									: "connect cancelled, connecting");
		}
		remote->read_thread_exited = true;
		/* This thread makes no further socket call; if the write thread is
		 * already done too, a parked fd becomes releasable right here. */
		release_pending_socket_locked(remote);
		thread_cond_broadcast(&remote->write_signal);
		thread_mutex_unlock(&remote->lock);
		return NULL;
	}
	remote->socket = connected;
	thread_mutex_lock(&remote->frame_lock);
	remote->w_socket = connected;
	thread_mutex_unlock(&remote->frame_lock);
	struct remote_event connected_event = {.kind = REMOTE_EVENT_CONNECTED,
										 .generation = generation};
	(void)push_event_locked(remote, &connected_event);
	thread_cond_broadcast(&remote->write_signal);
	thread_mutex_unlock(&remote->lock);

	/* S5.3 stack-gate fix: decoder + event staging live in the heap-resident
	 * impl (see server_remote_internal.h's read_* comment) -- the old stack
	 * locals summed to a 34,712-byte frame on this thread's 64KB LWP stack. */
	struct mp_decoder* decoder = &remote->read_decoder;
	mp_decoder_init(decoder);
	/* F5a (review finding H2): arm the body-allocation reclaim seam for this
	 * connection BEFORE the first byte -- the Packet51 refusal it answers can
	 * happen on the login burst's very first column. */
	remote_attach_decoder_reclaim(remote, decoder);
	struct mp_event* packet = &remote->read_packet;
	/* Heap-resident, REMOTE_READ_BUFFER_BYTES wide: the old 256-byte stack
	 * scratch made this loop drain the socket in ~800 IOS round trips per
	 * mc173 join burst, slow enough that the server's unbuffered
	 * non-blocking writes silently discarded the columns past ~15 -- the
	 * 2026-09-02 "world never reported loaded" root cause. Full writeup at
	 * the field's declaration (server_remote_internal.h). */
	uint8_t* const receive_buffer = remote->read_receive_buffer;
	uint8_t packet_bytes[MP_S0_FRAME_CAPACITY];
	size_t packet_length = 0;
	bool done = false;
	while(!done) {
		/* Fix A (2026-08-31, S1 re-review finding 6 closure -- crash-class,
		 * docs/mp_s1_hostile_review_2026-08-20.md:30): this loop used to go
		 * straight from a successful count>0 batch back into another
		 * net_socket_receive call with NO cancellation check anywhere in
		 * this path -- only the count<=0 branch below ever consulted
		 * stop_now. Every site in this file that closes `remote->socket`
		 * cross-thread (server_remote_cancel, the write thread's send-
		 * failure path, kill_connection_locked's callers in
		 * server_remote_chunks.c/_protocol.c) sets stop_now=true UNDER
		 * THIS SAME LOCK in the same critical section that shuts the
		 * socket down, so observing stop_now==true here under the lock is
		 * a hard guarantee that the shutdown/close has already happened --
		 * checking it immediately before every recv call (not only after
		 * one fails) means this thread never VOLUNTARILY issues a new recv
		 * on a socket some other thread has already torn down, closing the
		 * window where an OS-recycled fd number could otherwise be
		 * silently read from by this stale thread instead of erroring
		 * (server_remote_cancel's own comment covers why a reconnect's new
		 * connect cannot race in before this is observed). Any bytes
		 * already buffered by the CURRENT in-flight recv() when cancel
		 * fires still get decoded by the loop body below exactly as
		 * before -- but push_event_locked (and this same check on the very
		 * next iteration) refuse to act on them once stop_now is visible,
		 * so a cancel landing mid-batch still terminates this thread
		 * within one more iteration, never with an extra recv() call on a
		 * torn-down fd. graceful_stop is DELIBERATELY excluded here
		 * (matches the count<=0 branch's existing "expected" test below):
		 * a clean quit only half-closes the WRITE side
		 * (remote_write_thread's graceful branch), so this read loop must
		 * keep draining until the peer processes Packet255 and closes ITS
		 * side (detected below via count<=0), unchanged from before this
		 * fix. */
		thread_mutex_lock(&remote->lock);
		bool cancel_seen = remote->stop_now;
		thread_mutex_unlock(&remote->lock);
		if(cancel_seen)
			break;

		int count = net_socket_receive(connected, receive_buffer,
									   REMOTE_READ_BUFFER_BYTES);
		if(count > 0)
			/* Fix B: relaxed atomic add -- see the field's declaration
			 * comment (server_remote_internal.h) for why plain volatile
			 * was UB here, not just a torn read. */
			atomic_fetch_add_explicit(&remote->rx_bytes, (uint32_t)count,
									   memory_order_relaxed);
		if(count <= 0) {
			thread_mutex_lock(&remote->lock);
			/* terminal_reason_pending (mp-split-0912/5): a decoded Packet255
			 * already queued the authoritative reason, so this close is the
			 * EXPECTED second half of a kick -- publishing the transport's
			 * own EOF text here is what used to overwrite it. See the
			 * field's declaration comment (server_remote_internal.h). */
			bool expected = remote->stop_now || remote->graceful_stop
				|| remote->terminal_reason_pending;
			enum server_remote_state failed_state = remote->state;
			uint32_t rx = atomic_load_explicit(&remote->rx_bytes,
											   memory_order_relaxed);
			/* First-wins terminal latch; a kick already recorded mp_end=1. */
			if(remote->mp_end == 0) {
				remote->mp_end = count == 0 ? 2u : 3u;
				remote->mp_end_phase = (uint32_t)failed_state;
				remote->mp_end_rx = rx;
			}
			/* W6: and the TEXT, in this same critical section -- see
			 * latch_reason_locked. Built here rather than in the !expected
			 * block below because the EXPECTED exits are exactly the ones
			 * that left mp_end set with an empty reason. `local` separates
			 * the two truths a recv of 0 can carry: after a bare
			 * server_remote_cancel (stop_now with no graceful quit and no
			 * reason of its own) the FIN the reader saw is OUR
			 * close_socket_locked, so claiming "remote closed the socket"
			 * would blame the server for the player leaving. graceful_stop
			 * is NOT local in that sense -- a quit half-closes the write
			 * side only and then waits for the peer's own FIN, which is what
			 * this recv observed. First-wins keeps a kick text (or a chunk /
			 * codec diagnostic) ahead of both. */
			{
				char latched[128];
				bool local = remote->stop_now && !remote->graceful_stop;
				const char* latch_phase = remote_phase_name(failed_state);
				if(local)
					snprintf(latched, sizeof latched,
							 "local teardown closed the socket, %s, rx=%u, "
							 "gen=%u", latch_phase, rx, generation);
				else if(count == 0)
					snprintf(latched, sizeof latched,
							 "remote closed the socket, %s, rx=%u, gen=%u",
							 latch_phase, rx, generation);
				else
					snprintf(latched, sizeof latched,
							 "socket read failed (%d), %s, rx=%u, gen=%u",
							 count, latch_phase, rx, generation);
				latch_reason_locked(remote, latched);
			}
			thread_mutex_unlock(&remote->lock);
			if(!expected) {
				char reason[128];
				const char* phase = remote_phase_name(failed_state);
				/* Both messages keep their historic PREFIX verbatim
				 * ("remote closed the socket" / "socket read failed (%d)")
				 * -- test_s1_faults' FAULT_EOF_BODY case and the EOF
				 * ordering probes strstr/match on exactly those -- and both
				 * now carry the same three diagnostics: which protocol
				 * phase died, how many raw bytes this connection ever
				 * received, and WHICH connect generation it was (a
				 * reconnect storm otherwise produces identical text from
				 * different sessions). */
				if(count == 0)
					snprintf(reason, sizeof reason,
							 "remote closed the socket, %s, rx=%u, gen=%u",
							 phase, rx, generation);
				else
					snprintf(reason, sizeof reason,
							 "socket read failed (%d), %s, rx=%u, gen=%u",
							 count, phase, rx, generation);
				publish_error(remote, generation,reason);
			}
			break;
		}
		for(int i = 0; i < count; i++) {
			/* The trace ring records a bounded prefix for large Packet51 bodies;
			 * framing itself remains in the unbounded-by-recv incremental decoder. */
			if(packet_length < sizeof(packet_bytes))
				packet_bytes[packet_length++] = receive_buffer[i];
			enum mp_decode_result result
				= mp_decoder_feed(decoder, receive_buffer[i], packet);
			/* Fix B companion: publish this byte's decoder-phase sample
			 * via relaxed atomic stores right after mutating it -- see
			 * the dec_*_sample fields' declaration comment
			 * (server_remote_internal.h) for why get_status can no
			 * longer read read_decoder's own (non-atomic) fields
			 * directly. */
			atomic_store_explicit(&remote->dec_phase_sample, decoder->phase,
								   memory_order_relaxed);
			atomic_store_explicit(&remote->dec_packet_id_sample,
								   decoder->packet_id, memory_order_relaxed);
			atomic_store_explicit(&remote->dec_chunk_have_sample,
								   (uint32_t)decoder->chunk_have,
								   memory_order_relaxed);
			atomic_store_explicit(&remote->dec_chunk_need_sample,
								   (uint32_t)decoder->chunk_need,
								   memory_order_relaxed);
			if(result == MP_DECODE_ERROR) {
				/* F5a (review finding H2): the whole terminal lives in
				 * remote_read_decode_error now -- capacity latch, reason and
				 * protocol-error fallback -- so a test can drive the PRODUCTION
				 * diagnosis (the review's "the tests do not cover this path").
				 * Either way this read loop is finished. */
				(void)remote_read_decode_error(remote, decoder, generation);
				done = true;
				break;
			}
			if(result == MP_DECODE_EVENT) {
#ifdef PLATFORM_WII
				cw_decode_trace((int)packet->type);
				cw_lock_trace("read_lock_acquire", (int)packet->type);
#endif
				thread_mutex_lock(&remote->lock);
#ifdef PLATFORM_WII
				cw_lock_trace("read_lock_acquired", (int)packet->type);
#endif
				thread_mutex_lock(&remote->frame_lock);
				trace_locked(remote, remote->generation, false, packet_bytes,
							 packet_length);
				thread_mutex_unlock(&remote->frame_lock);
				bool retained = true;
				/* Dimension change in STREAM order (docs/mp_dimension_change_2026-09-11.md):
				 * Beta's server sends Packet9 and then the new dimension's
				 * Packet50/51 burst back to back. The consumer applies the
				 * Respawn event later, but chunk bodies are retained HERE, so
				 * the old world's entries must go now -- otherwise the new
				 * world's bodies chain onto stale same-coordinate columns
				 * ("remote Packet51 body chain exhausted", seen live against
				 * Poseidon 2026-09-11) or get wiped by the consumer's clear. */
				if(packet->type == MP_EVENT_LOGIN)
					remote->read_dimension = packet->payload.login.dimension;
				else if(packet->type == MP_EVENT_RESPAWN
						&& packet->payload.respawn_dimension
							   != remote->read_dimension) {
					remote->read_dimension = packet->payload.respawn_dimension;
					remote->world_epoch++;
					remote_chunks_clear_locked(remote);
				}
				if(packet->type == MP_EVENT_MAP_CHUNK)
					retained = remote_retain_map_locked(remote, packet);
				else if(packet->type == MP_EVENT_PRE_CHUNK)
					retained = remote_pre_chunk_locked(
						remote, packet->payload.pre_chunk.chunk_x,
						packet->payload.pre_chunk.chunk_z,
						packet->payload.pre_chunk.mode);
				else {
					struct remote_event* event = &remote->read_event;
					event->kind = REMOTE_EVENT_PACKET;
					event->generation = generation;
					event->world_epoch = remote->world_epoch;
					event->packet = *packet;
					/* Receive-order stamp, same counter as a retained
					 * Packet51 body (struct remote_event::seq). */
					event->seq = ++remote->arrival_sequence;
					retained = push_event_locked(remote, event);
					if(retained) {
						/* push_event_locked copies the struct shallowly: the
						 * queued entry now independently owns any heap
						 * pointer inside packet->payload. NULL this original's
						 * copies so the unconditional mp_event_destroy below
						 * does not free memory the queue now owns -- the same
						 * ownership-transfer idiom MAP_CHUNK's own path above
						 * already uses via remote_retain_map_locked. */
						if(packet->type == MP_EVENT_MULTI_BLOCK_CHANGE) {
							packet->payload.multi_block_change.coordinate = NULL;
							packet->payload.multi_block_change.type = NULL;
							packet->payload.multi_block_change.metadata = NULL;
						} else if(packet->type == MP_EVENT_WINDOW_ITEMS)
							packet->payload.window_items.items = NULL;
						/* Adversarial-review CRITICAL closure (found while
						 * building the explosion-truncate fixture test, not
						 * in the original review): S4 added MP_EVENT_EXPLOSION
						 * as a THIRD heap-owning event type (the `blocks`
						 * array), but this ownership-transfer list was never
						 * extended for it -- push_event_locked's shallow
						 * copy left the queued event's blocks pointer aliasing
						 * this local `packet` copy's, so the unconditional
						 * mp_event_destroy(&packet) below freed it once here
						 * and again in process_event's own mp_event_destroy
						 * call: a real double-free, reproduced as
						 * STATUS_HEAP_CORRUPTION by any live explosion packet
						 * (confirmed via test_s4_explosion_truncate; no prior
						 * fixture exercised MP_EVENT_EXPLOSION through this
						 * read-thread dispatch path -- the codec goldens only
						 * decode directly, never through here). */
						else if(packet->type == MP_EVENT_EXPLOSION)
							packet->payload.explosion.blocks = NULL;
					}
				}
				packet_length = 0;
				/* GUARD 1 of 2 (mp-split-0912/5): the kick is now safely in
				 * the pump's queue, so the server's reason is the
				 * authoritative one for this session. Latch that BEFORE
				 * unlocking (the next receive is usually already the peer's
				 * FIN/RST) and stop reading: a Packet255 is terminal, so
				 * nothing arriving after it can matter, and every further
				 * recv on this socket can only produce an EOF/error whose
				 * text would compete with the kick. `retained` gates it --
				 * if the push FAILED the reason never reaches the pump, so
				 * the old diagnostic path must still run (the !retained
				 * branch right below owns that case). */
				if(retained && packet->type == MP_EVENT_DISCONNECT) {
					remote->terminal_reason_pending = true;
					if(remote->mp_end == 0) {
						remote->mp_end = 1u;
						remote->mp_end_phase = (uint32_t)remote->state;
						remote->mp_end_rx = atomic_load_explicit(
							&remote->rx_bytes, memory_order_relaxed);
						/* W6: the kick TEXT is still only in the queued
							 * packet -- the pump's MP_EVENT_DISCONNECT handler
							 * transcodes it into reason when it reaches it. A
							 * teardown that beats the pump to it (exactly
							 * mp-split-0912/7's case: mp_session_release_remote
							 * destroyed the impl ~400 ms after a kick) would
							 * otherwise snapshot mp_end=1 with no text at all.
							 * This stand-in states WHAT happened; the handler
							 * overwrites it with the server's own words as soon
							 * as the pump drains the packet, so it is never what
							 * a pumped session shows the player. */
						char kicked[128];
						snprintf(kicked, sizeof kicked,
							 "kicked by the server, %s, rx=%u, gen=%u",
							 remote_phase_name(remote->state),
							 remote->mp_end_rx, generation);
						latch_reason_locked(remote, kicked);
					}
					done = true;
				}
				if(!retained) {
					/* The refusing retain path already set the specific
					 * reason; deliver it as an error (finding 1) BEFORE
					 * stop_now blocks the side-slot, so the pump can reach
					 * DISCONNECTED instead of a forever-PLAY freeze. */
					publish_error_locked(remote, generation, remote->reason);
					remote->stop_now = true;
					close_socket_locked(remote);
					thread_cond_broadcast(&remote->write_signal);
					done = true;
				}
#ifdef PLATFORM_WII
				cw_lock_trace("read_lock_release", (int)packet->type);
#endif
				thread_mutex_unlock(&remote->lock);
#ifdef PLATFORM_WII
				cw_lock_trace("read_lock_released", (int)packet->type);
				// S5.3 hunt PROBE (temporary, ROUND 6): scan the read_*
				// scratch guard canary right after the write it guards
				// (remote->read_event was just filled a few lines above,
				// right before push_event_locked). Read-thread-only file
				// (cw_mp_trace.txt), own small budget.
				cw_read_canary_scan(remote, "read_lock_released");
				// ROUND 7: the LWP stack-watermark scan that used to sit
				// here was removed alongside its paint (see
				// remote_read_thread's entry comment) -- read_scratch_canary
				// above is unaffected (heap-based struct field, not a raw
				// stack-address computation).
#endif
				mp_event_destroy(packet);
				if(done)
					break;
			}
		}
	}
	mp_decoder_destroy(decoder);

	thread_mutex_lock(&remote->lock);
	if(remote->graceful_stop)
		close_socket_locked(remote);
	remote->read_thread_exited = true;
	/* Last socket call on this thread is behind us: if the write thread has
	 * already flagged itself exited, the parked fd is releasable NOW -- the
	 * peer needs that close to get its RST, so this must not wait for cancel
	 * (see the "Socket teardown" comment's WHEN paragraph). */
	release_pending_socket_locked(remote);
	thread_cond_broadcast(&remote->write_signal);
	thread_mutex_unlock(&remote->lock);
	return NULL;
}

/* IN-009e: the write thread's terminal classification, at the producer.
 * Before this, the send-failure branch published a bare "socket write
 * failed" into the error side-slot and then set stop_now + shut the socket
 * down; the mp_end trio was left for someone else to fill in. The reader,
 * woken by THAT shutdown, latched mp_end 2/3 ("local teardown closed the
 * socket") -- or, if it saw stop_now before its next recv, nothing, and the
 * pump's side-slot latch then wrote 6 (protocol) -- so a write failure was
 * reported as one of its consequences (Sol IN-009 review P2). Here the code,
 * phase and EXACT rx_bytes are latched under the lock, before stop_now, so
 * the reader's own first-wins latch (mp_end == 0 guard) and the pump's
 * remote_latch_protocol_end_locked both become no-ops for this ending.
 *
 * Not a failure, so nothing latched (the error is still published exactly as
 * before, so teardown is unchanged): a stale generation, a session already
 * stopping (stop_now: a local cancel/kill shut the socket under this send),
 * a graceful quit (graceful_stop: the player leaving), or a decoded
 * Packet255 that owns the ending (terminal_reason_pending /
 * reason_from_disconnect / REMOTE_KICKED), or an earlier error already
 * pending in the side-slot (see the guard). First-wins on mp_end and on the
 * reason text, like every other producer. Caller holds remote->lock. */
static void remote_write_fail_locked(struct server_remote_impl* remote,
									 uint32_t generation, int count) {
	enum server_remote_state failed_state = remote->state;
	uint32_t rx = atomic_load_explicit(&remote->rx_bytes,
									   memory_order_relaxed);
	char reason[128];
	snprintf(reason, sizeof reason,
			 "socket write failed (%d), %s, rx=%u, gen=%u", count,
			 remote_phase_name(failed_state), rx, generation);
	bool failure = generation == remote->generation && !remote->stop_now
		&& !remote->graceful_stop && !remote->terminal_reason_pending
		&& !remote->reason_from_disconnect && failed_state != REMOTE_KICKED
		/* An EARLIER error of this generation (e.g. remote_read_decode_error's
		 * unsupported-packet publish, which sets neither mp_end nor stop_now)
		 * still occupies the side-slot: that is the original cause and this
		 * send failure is its consequence. publish_error_locked below drops
		 * this message, and the pump latches code, phase, rx and reason
		 * together from that first error -- latching 7 here would pair
		 * this code/rx with the earlier error's reason. */
		&& !(remote->error_pending
			 && remote->error_pending_generation == remote->generation);
	if(failure) {
		if(remote->mp_end == 0) {
			remote->mp_end = SERVER_REMOTE_END_SEND;
			remote->mp_end_phase = (uint32_t)failed_state;
			remote->mp_end_rx = rx;
		}
		latch_reason_locked(remote, reason);
	}
	publish_error_locked(remote, generation, reason);
}

static void* remote_write_thread(void* user) {
	// ROUND 6's LWP stack-floor paint (see remote_read_thread's identical
	// removal comment) was REMOVED in round 7 -- this thread's own frame is
	// small enough (~1,080 bytes) that the same miscomputation happened to
	// still land inside real stack, but it's the same defect class and was
	// dropped for consistency/trust, not because it visibly misbehaved here.
	/* REL-MP-FIX3e: wait, pop and post-send accounting take frame_lock
	 * only; impl->lock is taken solely on the two terminal paths (stop
	 * exit, send failure), after frame_lock is released. */
	struct server_remote_impl* remote = user;
	for(;;) {
		thread_mutex_lock(&remote->frame_lock);
		while(remote->frame_count == 0 && !remote->w_stop
			  && !remote->w_graceful)
			thread_cond_wait(&remote->write_signal, &remote->frame_lock);
		/* Snapshot for the SD trace: file I/O never runs under frame_lock. */
		uint32_t wake_count = (uint32_t)remote->frame_count;
		uint32_t wake_stop = (uint32_t)remote->w_stop;
		(void)wake_count;
		(void)wake_stop;
		if(remote->w_stop
		   || (remote->w_graceful && remote->frame_count == 0)) {
			thread_mutex_unlock(&remote->frame_lock);
#ifdef PLATFORM_WII
			cw_write_trace("write_wake", wake_count, wake_stop);
#endif
			thread_mutex_lock(&remote->lock);
			if(remote->stop_now) {
				close_socket_locked(remote);
				/* Frames for a failed generation are no longer sendable ownership;
				 * retire them so teardown/status cannot report a phantom backlog. */
				thread_mutex_lock(&remote->frame_lock);
				remote->frame_head = remote->frame_count = 0;
				thread_mutex_unlock(&remote->frame_lock);
			} else
				/* Half-close only after the complete Packet255 frame was sent. TCP
				 * orders its bytes before FIN; the peer can process the disconnect,
				 * then close its half so the continuously decoding reader exits. */
				net_socket_shutdown_write(remote->socket);
			remote->write_thread_exited = true;
			/* No further send() from this thread -- release the parked fd if
			 * the read thread is done too (see "Socket teardown"). */
			release_pending_socket_locked(remote);
			thread_mutex_unlock(&remote->lock);
			return NULL;
		}
		struct remote_frame frame = remote->frames[remote->frame_head];
		remote->frame_head = (remote->frame_head + 1) % REMOTE_QUEUE_CAPACITY;
		remote->frame_count--;
		net_socket_t socket = remote->w_socket;
		uint32_t generation = remote->w_generation;
		thread_mutex_unlock(&remote->frame_lock);
#ifdef PLATFORM_WII
		cw_write_trace("write_wake", wake_count, wake_stop);
		cw_write_trace("write_popped", (uint32_t)frame.frame.length,
					   (uint32_t)frame.frame.data[0]);
#endif

		if(frame.generation != generation || socket == NET_SOCKET_INVALID) {
#ifdef PLATFORM_WII
			cw_write_trace("write_stale_skip", frame.generation, generation);
#endif
			continue;
		}
		size_t sent = 0;
#ifdef PLATFORM_WII
		cw_write_trace("write_pre_send_loop", (uint32_t)frame.frame.length,
					   0);
#endif
		while(sent < frame.frame.length) {
			int count = net_socket_send(socket, frame.frame.data + sent,
										frame.frame.length - sent);
#ifdef PLATFORM_WII
			cw_write_trace("write_send_ret", (uint32_t)count,
						   (uint32_t)sent);
#endif
			if(count <= 0) {
				thread_mutex_lock(&remote->lock);
				/* IN-009e: classify + publish BEFORE stop_now/close, in this
				 * one critical section, so the teardown's receive EOF cannot
				 * be latched as the cause (remote_write_fail_locked). */
				remote_write_fail_locked(remote, frame.generation, count);
				remote->stop_now = true;
				close_socket_locked(remote);
				/* Second live-bug find (2026-08-19): this branch used to
				 * return without setting write_thread_exited, so any status-
				 * based wait (workers_exited(), used by server_remote_cancel
				 * callers and every reconnect/teardown test) could spin to
				 * its own timeout even though the OS thread had already
				 * returned -- the write half of a real socket failure never
				 * self-reported as exited. */
				remote->write_thread_exited = true;
				/* Same as the clean-exit branch above: this thread is past
				 * its last send(), so the fd it just parked can go back to
				 * the OS as soon as the read thread is out too. */
				release_pending_socket_locked(remote);
				thread_mutex_unlock(&remote->lock);
				return NULL;
			}
			sent += (size_t)count;
		}
#ifdef PLATFORM_WII
		cw_write_trace("write_send_loop_done", (uint32_t)sent, 0);
#endif
		thread_mutex_lock(&remote->frame_lock);
		trace_locked(remote, frame.generation, true, frame.frame.data,
					 frame.frame.length);
		if(frame.frame.data[0] == MP_PACKET_KEEP_ALIVE)
			remote->keep_alives_sent++;
		if(frame.correction_echo)
			remote->corrections_echoed++;
		thread_mutex_unlock(&remote->frame_lock);
#ifdef PLATFORM_WII
		cw_write_trace("write_iter_done", (uint32_t)frame.frame.data[0], 0);
#endif
	}
}

#ifdef CAVEWII_CLIENT_INTERFACE_TEST
bool server_remote_test_push_event(struct server_remote* remote,
                                  const struct remote_event* event) {
	struct server_remote_impl* impl = remote->impl;
	thread_mutex_lock(&impl->lock);
	bool ok = push_event_locked(impl, event);
	thread_mutex_unlock(&impl->lock);
	return ok;
}
#endif

bool server_remote_init(struct server_remote* remote) {
	if(!remote)
		return false;
	/* Like server_local, production may place this handle in uninitialized
	 * automatic storage. Initialization must not inspect stack garbage. */
	remote->impl = NULL;
	/* mp-split-0912/7: same reason, for the terminal-status snapshot the
	 * outer handle now carries -- and this is the clear that makes the
	 * snapshot per-impl: a NEW impl means the previous session's record is
	 * done being reported, so the next teardown's snapshot is the one that
	 * wins. Must run before any early return below, or a failed init could
	 * leave get_status reading stack garbage through last_status_valid. */
	server_remote_status_latch_clear(remote);
	struct server_remote_impl* impl = calloc(1, sizeof(*impl));
	if(!impl)
		return false;
	impl->socket = impl->w_socket = NET_SOCKET_INVALID;
	/* calloc gives 0, which is a VALID fd number -- this field must be
	 * initialized explicitly or the first teardown would close fd 0. */
	impl->pending_close_socket = NET_SOCKET_INVALID;
	impl->state = REMOTE_DISCONNECTED;
	/* W2: the retention budget IS the backing reservation. Configure it from
	 * `mp.backing_mib` (clamped inside mp_backing_configure to
	 * [MEM2_MP_BACKING_MIB_MIN, MEM2_MP_BACKING_MIB_MAX]) and take the
	 * effective capacity back as the cap, so a config value the clamp or the
	 * carve could not honour is never reported as if it had been. Done here,
	 * at server_remote_init, because the reservation is per-MP-session state
	 * and this is the one place that runs exactly once per session handle.
	 * 0 = "not set": the default MEM2_MP_BACKING_BYTES stands. */
	{
		int backing_mib
			= config_read_int(&gstate.config_user, "mp.backing_mib", 0);
		impl->retained_byte_cap = mp_backing_configure(
			backing_mib > 0 ? (size_t)backing_mib * 1024u * 1024u
							: MEM2_MP_BACKING_BYTES);
	}
#ifdef PLATFORM_WII
	// S5.3 hunt PROBE (temporary, ROUND 6): paint the read_* scratch guard
	// canary once, right after calloc -- see server_remote_internal.h's
	// field comment. impl is calloc'd once per server_remote_init, which
	// happens once for the lifetime of the mp_session handle (not re-armed
	// per connect), so this paints before the read thread ever starts.
	cw_canary_paint(&impl->read_scratch_canary);
#endif
	if(!thread_mutex_init(&impl->lock)) {
		free(impl);
		return false;
	}
	if(!thread_mutex_init(&impl->frame_lock)) {
		thread_mutex_destroy(&impl->lock);
		free(impl);
		return false;
	}
	if(!thread_cond_init(&impl->write_signal)) {
		thread_mutex_destroy(&impl->frame_lock);
		thread_mutex_destroy(&impl->lock);
		free(impl);
		return false;
	}
	if(!thread_cond_init(&impl->event_space)) {
		thread_cond_destroy(&impl->write_signal);
		thread_mutex_destroy(&impl->frame_lock);
		thread_mutex_destroy(&impl->lock);
		free(impl);
		return false;
	}
	impl->synchronization_ready = true;
	if(!net_socket_platform_init(impl->reason, sizeof(impl->reason))) {
		thread_cond_destroy(&impl->event_space);
		thread_cond_destroy(&impl->write_signal);
		thread_mutex_destroy(&impl->frame_lock);
		thread_mutex_destroy(&impl->lock);
		free(impl);
		return false;
	}
	impl->platform_ready = true;
	remote->impl = impl;
	return true;
}

static void join_workers(struct server_remote_impl* remote) {
	if(remote->write_thread_started) {
		thread_join(&remote->write_thread);
		remote->write_thread_started = false;
	}
	if(remote->read_thread_started) {
		thread_join(&remote->read_thread);
		remote->read_thread_started = false;
	}
}

/* push_event_locked/pop_event_locked transfer ownership of any heap array
 * inside event.packet (MP_EVENT_MULTI_BLOCK_CHANGE's coordinate/type/
 * metadata, MP_EVENT_WINDOW_ITEMS's items) to whichever ring slot holds the
 * struct. A bare `head = count = 0` reset abandons that ownership -- any
 * entry the read thread queued but server_remote_pump never got to drain
 * before a disconnect/reconnect leaks its array. Call this instead of the
 * bare reset on every path that clears the ring; must run under impl->lock
 * (matches every other *_locked helper here). */
static void drain_event_queue_locked(struct server_remote_impl* remote) {
	for(size_t i = 0; i < remote->event_count; i++) {
		size_t index = (remote->event_head + i) % REMOTE_QUEUE_CAPACITY;
		mp_event_destroy(&remote->events[index].packet);
	}
	remote->event_head = remote->event_count = 0;
	thread_cond_broadcast(&remote->event_space); /* REL-MP-FIX5 */
}

void server_remote_cancel(struct server_remote* remote) {
	if(!remote || !remote->impl)
		return;
	struct server_remote_impl* impl = remote->impl;
	thread_mutex_lock(&impl->lock);
	impl->stop_now = true;
	/* Fix A part 2 (close-after-join, docs/mp_s1_hostile_review_2026-08-20.md:30
	 * re-review finding 6): this used to be a bare shutdown+close here,
	 * BEFORE join_workers below had confirmed the read/write threads
	 * actually stopped touching the fd. The shutdown-now / close-after-join
	 * split it grew instead is now close_socket_locked +
	 * close_pending_socket_after_join, shared by EVERY close path in this
	 * file (2026-09-01 audit item 3 -- the non-cancel closers still had the
	 * original defect); see the helpers' block comment for the full
	 * argument. Behaviour here is unchanged: impl->socket goes INVALID at
	 * exactly this point, the shutdown unblocks any in-flight recv()/send()
	 * at exactly this point, and the fd number is released at exactly the
	 * same place below -- after the join.
	 *
	 * The only call site that can hand out a new fd for this session is
	 * server_remote_start's connector, and start() refuses to run until
	 * impl->state == REMOTE_DISCONNECTED -- which this function only sets
	 * further down, AFTER join_workers returns -- so no fast reconnect can
	 * be handed a number a worker of this session might still touch. A
	 * recv() already in flight when this runs still completes off the
	 * (merely shut down, not yet freed) socket and is decoded once as
	 * before; the read loop's own pre-recv stop_now check then stops it
	 * from ever recv()-ing again. */
	close_socket_locked(impl);
	thread_cond_broadcast(&impl->write_signal);
	thread_mutex_unlock(&impl->lock);
	join_workers(impl);
	/* Both workers have now actually returned (thread_join is synchronous),
	 * so neither can issue another recv()/send() on this fd -- safe to
	 * release the number to the OS now. Usually a no-op: a session whose
	 * workers already exited released its fd in their own exit blocks. This
	 * is the BACKSTOP for the cases where they did not -- cancel-during-
	 * connect, or a worker still live when the caller tore the session
	 * down -- so no session can end with a number still parked. */
	close_pending_socket_after_join(impl);
	thread_mutex_lock(&impl->lock);
	remote_chunks_clear_locked(impl);
	remote_entities_clear(impl);
	/* Map mirrors are session state like the entity table: a later
	 * connection's map ids are unrelated, and a stale dirty flag must not
	 * push an old server's colours into the new session's held map. The
	 * counters persist (session totals, reconnect-stress precedent). */
	memset(impl->map_mirrors, 0, sizeof(impl->map_mirrors));
	impl->map_retry_count = 0;
	if(impl->climate) {
		worldgen_destroy(impl->climate);
		impl->climate = NULL;
	}
	impl->state = REMOTE_DISCONNECTED;
	drain_event_queue_locked(impl);
	thread_mutex_lock(&impl->frame_lock);
	impl->frame_head = impl->frame_count = 0;
	thread_mutex_unlock(&impl->frame_lock);
	impl->error_pending = false;
	thread_mutex_unlock(&impl->lock);
	/* F5a (review finding M9): GIVE THE RESERVATION BACK.
	 *
	 * Both workers are joined, remote_chunks_clear_locked above has freed
	 * every retained body and the queued chains with them, and the decoder's
	 * own in-flight body went with mp_decoder_destroy in the read thread's
	 * exit -- so nothing of this session is left inside the reservation or
	 * its scratch slots, and the 6 MiB (Wii default) it holds can go back to
	 * the carve. W2 left it carved for the life of the process like a pool
	 * slab: MP -> title -> load a local world then ran permanently 6 MiB
	 * short of the carve that meshes, columns and the far ring come out of,
	 * on the one platform where that budget is already the binding
	 * constraint.
	 *
	 * OUTSIDE the lock on purpose: mp_backing_release_if_idle takes the
	 * allocator's own lock (the shared mem2 mutex on the Wii), and no lock
	 * order between it and impl->lock is established anywhere else -- the
	 * allocator is only ever called with impl->lock held, never the reverse,
	 * and this call keeps it that way. Safe here because the session is
	 * already REMOTE_DISCONNECTED with both workers joined, so no thread can
	 * allocate a body between the unlock and this line.
	 *
	 * It is a no-op (returns 0) if anything is still live -- a second session
	 * in the same process, or a leaked body for the leak check to catch --
	 * which is why this is release_if_idle and not the asserting reset. */
	(void)mp_backing_release_if_idle();
}

void server_remote_destroy(struct server_remote* remote) {
	if(!remote || !remote->impl)
		return;
	struct server_remote_impl* impl = remote->impl;
	/* mp-split-0912/7: latch the terminal diagnostic into the outer handle
	 * BEFORE anything tears the impl down, so a heartbeat sampled after
	 * mp_session_release_remote still reports how the session ended instead
	 * of the impl-less zero fill. It must come before server_remote_cancel
	 * as well as before the free: cancel finalizes impl->state to
	 * REMOTE_DISCONNECTED, so capturing after it would erase a KICKED /
	 * CLOSING terminal state that had not been finalized yet. */
	server_remote_status_latch_capture(remote);
	server_remote_cancel(remote);
	if(impl->platform_ready)
		net_socket_platform_cleanup();
	if(impl->synchronization_ready) {
		thread_cond_destroy(&impl->event_space);
		thread_cond_destroy(&impl->write_signal);
		thread_mutex_destroy(&impl->frame_lock);
		thread_mutex_destroy(&impl->lock);
	}
	free(impl);
	remote->impl = NULL;
}

bool server_remote_start(struct server_remote* remote, const char* host,
						 uint16_t port, const char* username_utf8) {
	if(!remote || !remote->impl)
		return false;
	struct mp_frame validation;
	struct server_remote_impl* impl = remote->impl;
	thread_mutex_lock(&impl->lock);
	if(impl->state != REMOTE_DISCONNECTED || impl->read_thread_started
	   || impl->write_thread_started) {
		/* A live/tearing-down session owns the status; do not stomp its
		 * reason or generation for a mistimed second start. */
		thread_mutex_unlock(&impl->lock);
		return false;
	}
	/* mp-split-0912/7: this start is past its only rejection-without-a-
	 * generation-bump gate, so from here every path bumps the generation --
	 * i.e. a NEW session's record begins and the snapshot of the previous one
	 * is done being reported. Re-asserting the invariant here (it is already
	 * false for every live impl, because server_remote_init cleared it and
	 * only server_remote_destroy sets it) is what guarantees a stale
	 * terminal record can never outlive the generation it describes, whatever
	 * order a future caller mixes init/start/destroy in. Takes no lock. */
	server_remote_status_latch_clear(remote);
	/* M6 (2026-08-20 redesign, finding 6): every user-reachable rejection
	 * FAILS LOUD -- reason + generation bump, state left DISCONNECTED --
	 * so the connect screen's baseline-generation logic reports it instead
	 * of spinning "Connecting..." forever. Port 0 is parse-legal
	 * (mp_address_parse's golden even pins "example.com:0") and used to
	 * fall through this gate silently. */
	const char* rejection = NULL;
	if(!host || !*host || !username_utf8 || !*username_utf8)
		rejection = "empty server address or username";
	else if(port == 0)
		rejection = "port 0 is not connectable";
	else if(strlen(host) >= sizeof(impl->host))
		rejection = "server address too long";
	else if(strlen(username_utf8) >= sizeof(impl->username))
		rejection = "username too long";
	/* Packet1's 16-code-unit maximum is stricter than Packet2's 32.
	 * [740c583 Packet1Login.readPacketData]
	 * [740c583 Packet2Handshake.readPacketData] */
	else if(!mp_encode_login(&validation, username_utf8))
		rejection = "username not loginable";
	if(rejection) {
		set_reason_locked(impl, rejection);
		impl->generation++;
		if(impl->generation == 0)
			impl->generation = 1;
		thread_cond_broadcast(&impl->event_space); /* REL-MP-FIX5 */
		thread_mutex_unlock(&impl->lock);
		return false;
	}
	snprintf(impl->host, sizeof(impl->host), "%s", host);
	snprintf(impl->username, sizeof(impl->username), "%s", username_utf8);
	impl->port = port;
	impl->generation++;
	if(impl->generation == 0)
		impl->generation = 1;
	thread_cond_broadcast(&impl->event_space); /* REL-MP-FIX5 */
	impl->state = REMOTE_CONNECTING;
	impl->stop_now = false;
	impl->graceful_stop = false;
	impl->read_thread_exited = false;
	impl->write_thread_exited = false;
	impl->logged_in = false;
	// FIX (Codex adversarial review P1-9, real bug -- not a probe, landed
	// alongside this round's diagnostics): login_delay_pending/deadline were
	// never reset here, only cleared when the deadline actually fires
	// (server_remote.c). A cancel after handshake but before the delayed
	// send, followed by a reconnect, could carry the OLD latch into the NEW
	// connection: it can queue login during the new REMOTE_CONNECTING state
	// and advance straight to REMOTE_LOGIN_SENT before the real handshake
	// ever completes, silently corrupting the new session's state machine.
	// The current build's CW_LOGIN_DELAY_MS is 0 (no live exposure yet), but
	// the defect is real and becomes live the instant that diagnostic delay
	// is ever raised above 0 again. login_delay_deadline itself does NOT
	// need resetting: every read of it (server_remote.c) is short-circuited
	// behind login_delay_pending, so a stale deadline value is inert once
	// pending is false -- and ptime_t is `struct timespec` on PC, so a bare
	// `= 0` initializer would not even compile there.
	impl->login_delay_pending = false;
	impl->entity_id = 0;
	impl->map_seed = 0;
	impl->dimension = 0;
	impl->read_dimension = 0;
	impl->world_epoch = 0;
	impl->applied_epoch = 0;
	impl->keep_alives_received = 0;
	impl->arrival_sequence = 0;
	impl->have_authoritative_position = false;
	impl->have_authoritative_look = false;
	impl->authoritative_on_ground = false;
	impl->have_last_sent_position = false;
	impl->have_last_sent_look = false;
	/* Beta's EntityClientPlayerMP.wasSneaking starts false (Java default),
	 * so the first sendMotionUpdates tick sends Packet19 only if the player
	 * is ALREADY sneaking at spawn -- not unconditionally on first send,
	 * unlike this connection's position/look tracking above. Seed the known
	 * baseline as false/known rather than "unknown, always emit first".
	 * [740c583 EntityClientPlayerMP.sendMotionUpdates] */
	impl->last_sent_sneaking = false;
	impl->have_last_sent_sneaking = true;
	memset(&impl->sign_edit, 0, sizeof(impl->sign_edit));
	impl->progress_window = -1;
	impl->active_window = 0;
	impl->progress_cook = impl->progress_burn = impl->progress_fuel = 0;
	impl->correction_seq = 0;
	impl->load_completion_pending = false;
	impl->remote_load_complete_sent = false;
	impl->retained_bytes_peak = 0;
	impl->retained_entry_peak = 0;
	impl->inflated_bytes_frame = 0;
	impl->retention_evictions = 0;
	impl->retention_allocation_failures = 0;
	impl->promotion_inflate_init_failures = 0;
	impl->reconnect_recoveries = 0;
	impl->deferred_edits = 0;
	impl->deferred_edits_peak = 0;
	impl->deferred_edits_applied = 0;
	impl->deferred_edits_dropped = 0;
	impl->deferred_edits_superseded = 0;
	impl->deferred_edit_log_exhausted = 0;
	impl->chain_rejections = 0;
	impl->promotion_publish_waits = 0;
	/* W1 (design section 2.1): the admission radius is fixed PER CONNECTION.
	 * The faithful-to-today default is the full view distance
	 * (SERVER_REMOTE_ADMIT_RADIUS_MAX == MAX_VIEW_DISTANCE); main.c's
	 * server_remote_set_admit_radius call right after a successful start
	 * applies the "mp.install_radius" dial on top, so a stale radius from a
	 * previous session can never leak into a fresh one. */
	impl->admit_radius = SERVER_REMOTE_ADMIT_RADIUS_MAX;
	impl->demotions = 0;
	impl->demote_bytes = 0;
	impl->demotion_failures = 0;
	impl->forced_reconnects = 0;
	impl->event_backpressure_waits = 0;
	impl->events_deferred_frames = 0;
	impl->queued_events_peak = 0;
	impl->pump_gap_max_ms = 0;
	impl->have_last_pump = false;
	impl->error_pending = false;
	/* Per-generation terminal state (mp-split-0912/5): the kick latches and
	 * the mp_end trio describe THIS connection only. Clearing them here is
	 * what makes a reconnect start with a blank reason (the ordering suite's
	 * reconnect_clears_kick_reason case) instead of carrying the previous
	 * session's kick into the fresh one -- reason[] itself is cleared four
	 * lines below for exactly the same purpose. */
	impl->terminal_reason_pending = false;
	impl->reason_from_disconnect = false;
	impl->errors_dropped_after_kick = 0;
	impl->mp_end = 0;
	impl->mp_end_phase = 0;
	impl->mp_end_rx = 0;
	impl->corrections_received = 0;
	impl->reason[0] = '\0';
	/* By construction this is always empty here (state == REMOTE_DISCONNECTED
	 * implies server_remote_cancel already ran and drained it), but reset
	 * through the same draining helper rather than a bare zero -- defense in
	 * depth against the same leak if that invariant is ever broken. */
	drain_event_queue_locked(impl);
	thread_mutex_lock(&impl->frame_lock);
	impl->frame_head = impl->frame_count = 0;
	impl->trace_head = impl->trace_count = 0;
	impl->keep_alives_sent = 0;
	impl->corrections_echoed = 0;
	impl->w_socket = NET_SOCKET_INVALID;
	impl->w_generation = impl->generation;
	impl->w_stop = impl->w_graceful = false;
	thread_mutex_unlock(&impl->frame_lock);
	thread_mutex_unlock(&impl->lock);

	/* Owner: server_remote. Stop: stop_now/graceful_stop + socket shutdown.
	 * Join: server_remote_cancel/destroy after writer, then reader. */
	if(!thread_create(&impl->read_thread, remote_read_thread, impl, 64)) {
		thread_mutex_lock(&impl->lock);
		impl->state = REMOTE_DISCONNECTED;
		set_reason_locked(impl, "read thread creation failed");
		thread_mutex_unlock(&impl->lock);
		return false;
	}
	impl->read_thread_started = true;
	if(!thread_create(&impl->write_thread, remote_write_thread, impl, 64)) {
		thread_mutex_lock(&impl->lock);
		impl->stop_now = true;
		close_socket_locked(impl);
		thread_mutex_unlock(&impl->lock);
		/* join_workers rather than a hand-rolled join: write_thread_started
		 * is still false, so it joins exactly the read thread -- and the
		 * parked fd must be released HERE, through the same join-then-close
		 * ordering as cancel. This path leaves state DISCONNECTED without
		 * going through server_remote_cancel, so it is the one other place
		 * that owns a parked fd; failing to drain it would strand the number
		 * until destroy and trip close_socket_locked's one-per-session
		 * assert on the next start(). */
		join_workers(impl);
		close_pending_socket_after_join(impl);
		thread_mutex_lock(&impl->lock);
		impl->state = REMOTE_DISCONNECTED;
		set_reason_locked(impl, "write thread creation failed");
		thread_mutex_unlock(&impl->lock);
		return false;
	}
	impl->write_thread_started = true;
	return true;
}

bool server_remote_send_keep_alive(struct server_remote* remote) {
#ifdef PLATFORM_WII
	cw_decode_trace(-100); // S5.3 hunt PROBE (round 3 cont.): entry marker,
							// reuses the decode-trace budget/file with a
							// sentinel type so it interleaves in order.
#endif
	if(!remote || !remote->impl)
		return false;
	struct mp_frame frame;
	if(!mp_encode_keep_alive(&frame))
		return false;
	struct server_remote_impl* impl = remote->impl;
	thread_mutex_lock(&impl->lock);
	bool connected = impl->socket != NET_SOCKET_INVALID
		&& (impl->state == REMOTE_HANDSHAKE_SENT
			|| impl->state == REMOTE_LOGIN_SENT || impl->state == REMOTE_PLAY);
	bool queued = connected && queue_frame_locked(impl, &frame, false);
	thread_mutex_unlock(&impl->lock);
#ifdef PLATFORM_WII
	cw_decode_trace(-101); // exit marker
#endif
	return queued;
}

bool server_remote_send_chat(struct server_remote* remote,
							 const char* message_utf8) {
	/* S2 harness/diagnostic primitive, NOT the S4 chat composer: the plan's
	 * S4 slice still owns the player-facing send UI/history (mp_codec.h's
	 * own mp_encode_chat comment: "No composer wires this in this slice").
	 * This just gives the ALREADY-BIDIRECTIONAL, already-tested Packet3
	 * encode (mp_packet_supported reports MP_PACKET_CHAT bidirectional) a
	 * production entry point, mirroring server_remote_send_keep_alive's own
	 * direct-encode-and-queue shape. Exercised today by the deterministic
	 * mc173 peer harness (tests/test_mp_s0_env.c's run_live_s2) to issue
	 * server-side setup commands (e.g. "/give") the S2 GATE's dig/place/
	 * chest/furnace cycles need real inventory contents for -- mc173 has no
	 * item-spawn command outside chat, and Beta's own protocol has no other
	 * legal channel for it [740c583 Packet3Chat]. */
	if(!remote || !remote->impl || !message_utf8)
		return false;
	/* Outbound sanitize at the production choke point (betamc-readiness
	 * "Outbound chat" row): Beta's composer cap is 100 with \247/controls
	 * untypeable [740c583 GuiChat:40], and CB-lineage servers KICK on
	 * either violation -- see mp_chat_sanitize_outbound's contract. The
	 * live-harness "/give" setup commands are plain ASCII and pass
	 * through unchanged. */
	char sanitized[401];
	if(mp_chat_sanitize_outbound(message_utf8, sanitized, sizeof(sanitized))
	   == 0)
		return false;
	struct mp_frame frame;
	if(!mp_encode_chat(&frame, sanitized))
		return false;
	struct server_remote_impl* impl = remote->impl;
	thread_mutex_lock(&impl->lock);
	bool connected = impl->socket != NET_SOCKET_INVALID
		&& impl->state == REMOTE_PLAY;
	bool queued = connected && queue_frame_locked(impl, &frame, false);
	thread_mutex_unlock(&impl->lock);
	return queued;
}

bool server_remote_disconnect(struct server_remote* remote,
							  const char* reason_utf8) {
	if(!remote || !remote->impl || !reason_utf8)
		return false;
	struct server_remote_impl* impl = remote->impl;
	struct mp_frame frame;
	if(!mp_encode_disconnect(&frame, reason_utf8))
		return false;
	thread_mutex_lock(&impl->lock);
	if(impl->state == REMOTE_CONNECTING) {
		thread_mutex_unlock(&impl->lock);
		/* No socket exists yet, so there is no peer that can receive Packet255.
		 * Cancellation owns the connect worker and joins it instead. */
		server_remote_cancel(remote);
		return true;
	}
	bool queued = queue_frame_locked(impl, &frame, false);
	if(queued) {
		impl->graceful_stop = true;
		impl->state = REMOTE_CLOSING;
		thread_mutex_lock(&impl->frame_lock);
		impl->w_graceful = true;
		thread_cond_broadcast(&impl->write_signal);
		thread_mutex_unlock(&impl->frame_lock);
	}
	thread_mutex_unlock(&impl->lock);
	return queued;
}

#endif /* CAVEWII_MP_CLIENT */
