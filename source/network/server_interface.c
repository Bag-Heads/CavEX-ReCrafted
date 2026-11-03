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
#include <stdint.h>
#include <string.h>

#include "../platform/thread.h"
#include "server_interface.h"

/* host_vehicle_smoke_8m ingress witness (vehicle_smoke_probe.c installs it
 * on arm and clears it on disarm). A plain pointer, not a probe call, so this
 * file keeps linking standalone (test_svin_reliable). NULL = dormant: one
 * load, no other effect. Called on the calling (main) thread only. */
void (*svin_vsp_ingress_observer)(const struct server_rpc* call, uint8_t slot,
	uint64_t generation, bool admitted);

#define RPC_INBOX_SIZE 16
static struct server_rpc rpc_msg[RPC_INBOX_SIZE];
static struct server_rpc_source rpc_source[RPC_INBOX_SIZE];
/* v5: messages held by the admitted drain (see svin_process_owned_messages_admitted). */
static struct server_rpc* svin_held[RPC_INBOX_SIZE];
static size_t svin_held_n;
/* V6 F1: the progress-route stamp of each token (0 = none), the stamp of the
 * message being dispatched right now, and the cancelled-message count. */
static uint32_t rpc_ctx[RPC_INBOX_SIZE];
static uint32_t svin_cur_ctx;
static uint32_t svin_cancelled;
static struct thread_channel svin_inbox;
static struct thread_channel svin_empty_msg;
static uint32_t svin_active_rpc = 0;

uint32_t svin_active_owned_rpc_marker(void) {
	return __atomic_load_n(&svin_active_rpc, __ATOMIC_RELAXED);
}

// Count of client->server messages DROPPED under a full inbox (see svin_rpc_send).
// Since the reliable-delivery split, only SRPC_PLAYER_POS is normally dropped
// (it is periodic + idempotent: the next 50ms tick re-sends), plus bounded
// reserve/pending-overflow rejection below. Player ACTIONS are queued, never
// dropped. Surfaced on the F3 far row ("rpcdrop") so rejection is never silent.
// Read via svin_dropped_count().
static uint32_t svin_dropped = 0;

/* Experimental local split-screen accept gate. OFF is the shipped default and
 * the state a fresh queue starts in (svin_init), so the slot-1 filter below is
 * byte-identical to its pre-split-screen self: an SRPC_PLAYER_POS from an
 * ATTACHED guest is refused AT THE QUEUE BOUNDARY and consumes neither an inbox
 * token nor a dispatch.
 *
 * CROSS-THREAD (R-A8 A8-M5). The LIVE switch (Astra option A, S-A1) made this
 * a genuinely shared flag: it is WRITTEN on the SERVER thread -- by
 * server_local_split_apply out of the SRPC_SPLIT_SET handler and by
 * server_local_split_policy_apply at guest admit/release -- and READ on the
 * MAIN/client thread in svin_rpc_send_from's slot-1 filter below. (The boot
 * setter still writes it on the main thread before the server thread starts;
 * that is now the exception, not the rule.) A plain bool there is a data race:
 * benign on the single-core Wii LWP scheduler, but UB on the SMP PC build,
 * where the compiler may keep the read hoisted out of the filter. So it is
 * accessed with the same explicit __atomic_* accessors every other
 * cross-thread flag in this layer uses (cf. s->thread_done in server_local.c).
 * RELAXED is the right order: the flag carries no data with it -- the server
 * re-validates the generation against an actually-admitted body before acting
 * on any message this gate lets through -- so all that is needed is a single
 * indivisible read of the latest published value, not a barrier. */
static bool svin_split_accept = false;

void svin_set_split_accept(bool enabled) {
	__atomic_store_n(&svin_split_accept, enabled, __ATOMIC_RELAXED);
}

uint32_t svin_dropped_count(void) {
	return svin_dropped;
}

// Reliable-delivery pending queue (FIFO ring). Real-Wii playtest 2026-06-07
// falsified the old "drops occur only transiently during the initial stream
// flood" claim: under in-play lag the inbox filled and dig/place actions were
// dropped -- the player saw "blocks are not mined". Beta's client->server
// channel is TCP: actions are DELAYED under load, never lost. So: when the
// inbox is full, actions land here and are flushed (in order) by
// svin_rpc_flush_pending from clin_update each frame and from every later
// send. THREADING: svin_rpc_send and the flush run on the client/main thread
// only (input handlers + clin_update); the queue is deliberately unlocked.
#define SVIN_ACTION_PENDING_SIZE 64
// CLOSE/UNLOAD/LOAD, the respawn acknowledgement and a live voxel-stream
// recovery must remain FIFO behind all preceding actions, but cannot evict one
// to make room. Keep their capacities independent: a full 64-action backlog
// plus the legal close -> unload -> load burst still has guaranteed ownership
// control slots.
#define SVIN_LIFECYCLE_RESERVE 3
#define SVIN_GUEST_CLOSE_RESERVE 1
#define SVIN_INPUT_ACK_RESERVE 1
#define SVIN_STREAM_RECOVER_RESERVE 1
#define SVIN_PLAYER_STATE_RESERVE 1
/* S1-a2 / Astra gate-2 D4. The movement-STATE slot above is ONE slot, and the
 * coalescing branch below only ever replaces a pending entry from the SAME
 * source -- so once PLAYER_STATE joined the slot-1 accept class, whichever
 * owner reached that slot first would have starved the other owner's
 * sneak/on-ground edge outright. A guest whose sneak never registers is
 * exactly the one-owner-starves-the-other defect D4 names. Give the guest its
 * own dedicated slot, the same shape (and for the same reason) as
 * SVIN_GUEST_CLOSE_RESERVE beside the lifecycle reserve: the census becomes
 * per-slot and the total grows by that one guaranteed control slot instead of
 * the two owners fighting over one. The ACTION class total below is untouched. */
#define SVIN_GUEST_PLAYER_STATE_RESERVE 1
/* The eight GAMEPLAY verbs share the unreserved ordinary class above, which
 * had NO per-owner bound: a 64-deep host action train filled it and the next
 * guest action was rejected (and vice versa). D4 requires a per-owner
 * reservation with the BOUNDED TOTAL UNCHANGED, so this is a per-owner CAP
 * inside the existing 64, not extra capacity: while the split accept class is
 * open, neither owner may hold more than SVIN_ACTION_PENDING_SIZE minus
 * SVIN_ACTION_OWNER_RESERVE ordinary entries, which leaves
 * SVIN_ACTION_OWNER_RESERVE guaranteed for the other one.
 *
 * Why the HOST cap is conditioned on the accept flag: with the split switch
 * OFF (the shipped default, and the state svin_init starts in) slot 1 cannot
 * enqueue an ordinary action at all -- the slot-1 filter above refuses the
 * whole class at the queue boundary -- so reserving anything for it would only
 * narrow the host's own allowance from 64 to 48 for no benefit, and would
 * change the SHIPPED single-player queue behaviour. Conditioning it keeps the
 * OFF path byte-identical (the existing test_svin_pending_equiv cases are that
 * golden) and arms the reservation exactly when contention becomes possible.
 * The guest cap is unconditional in the other direction: slot 0 can always
 * enqueue, so the host's reserve always holds. */
#define SVIN_ACTION_OWNER_RESERVE 16
#define SVIN_MINECART_CONTROL_RESERVE 1
#define SVIN_RIDE_INTENT_RESERVE 1
/* The LIVE split switch is an owner MENU action, not gameplay: it must not
 * be swallowed by a full ordinary-action backlog (the same reasoning as the
 * ride intent above), and 50 impatient toggles must stay bounded. One slot
 * plus the desired-state coalescing below is enough -- only the newest
 * request for a given world generation can matter. */
#define SVIN_SPLIT_SET_RESERVE 1
/* V6 F1 (ASTRA-V5-REVIEW R1): LOCAL movement samples are lossless. The local
 * server replays ONE physics step per accepted SRPC_PLAYER_POS (Beta's one
 * onUpdateEntity per Packet10), so a sample the transport drops is a lost
 * physics step, not a stale pose. In local-motion mode a sample that finds
 * no inbox token waits here, FIFO with every other class, in a bounded
 * per-OWNER reserve; the producer asks svin_motion_room BEFORE it authors,
 * so when the reserve is full the client authors nothing (backpressure)
 * instead of authoring and losing. 64 per owner is ~3.2 s of 20 Hz samples
 * beyond the 16 server tokens; bit-exact either way (only WHEN the producer
 * waits depends on it), a tuning candidate pending real-Wii observation.
 * Remote mode never uses it: there a full-inbox PLAYER_POS is still dropped. */
#define SVIN_LOCAL_MOTION_RESERVE 62
/* V6 F1 round 3 (ASTRA-F1-DESIGN-RULING 8): pause/resume and the local
 * guest mailbox (join/leave/cancel) stay enqueueable when the ordinary action
 * class is full. Two per owner (a pause+resume pair), carved out of the motion
 * reserve above (64 -> 62) so the ring size -- and Wii BSS -- is unchanged. */
#define SVIN_CONTROL_RESERVE 2
#define SVIN_PENDING_SIZE \
	(SVIN_ACTION_PENDING_SIZE + SVIN_LIFECYCLE_RESERVE + SVIN_GUEST_CLOSE_RESERVE \
	 + SVIN_INPUT_ACK_RESERVE + SVIN_STREAM_RECOVER_RESERVE \
	 + SVIN_PLAYER_STATE_RESERVE + SVIN_GUEST_PLAYER_STATE_RESERVE + SVIN_MINECART_CONTROL_RESERVE \
	 + SVIN_RIDE_INTENT_RESERVE + SVIN_SPLIT_SET_RESERVE \
	 + 2 * SVIN_LOCAL_MOTION_RESERVE + 2 * SVIN_CONTROL_RESERVE)
static struct server_rpc svin_pending[SVIN_PENDING_SIZE];
static struct server_rpc_source svin_pending_source[SVIN_PENDING_SIZE];
static size_t svin_pending_head = 0;
static size_t svin_pending_len = 0;
static size_t svin_pending_lifecycle_count[2] = {0};
static size_t svin_pending_input_ack_count = 0;
static size_t svin_pending_stream_recover_count = 0;
static size_t svin_pending_player_state_count[2] = {0};
/* Per-owner ORDINARY (gameplay-action) census. Maintained alongside the
 * derived total so the per-owner cap is O(1) rather than a queue walk. */
static size_t svin_pending_action_count[2] = {0};
static size_t svin_pending_minecart_control_count = 0;
static size_t svin_pending_ride_intent_count = 0;
static size_t svin_pending_split_set_count = 0;
/* Per-owner local motion census (V6 F1) and its mode flag. */
static size_t svin_pending_motion_count[2] = {0};
static uint32_t svin_motion_refused = 0;
static bool svin_local_motion_on = false;

enum convert_phase {
	CONVERT_IDLE, CONVERT_QUEUED, CONVERT_RUNNING,
	CONVERT_CANCEL_QUEUED, CONVERT_CANCEL_RUNNING,
	CONVERT_FINISHING, CONVERT_FINISHED
};
static unsigned convert_phase;
static uint64_t convert_next_ticket;
static struct svin_convert_result convert_result;
static char convert_path[SVIN_CONVERT_PATH_MAX];

uint64_t svin_convert_request(const char* path) {
	if(!svin_local_motion_on || !path || !path[0]
	   || strlen(path) >= sizeof convert_path || convert_next_ticket == UINT64_MAX
	   || __atomic_load_n(&convert_phase, __ATOMIC_ACQUIRE) != CONVERT_IDLE)
		return 0;
	strcpy(convert_path, path);
	convert_result = (struct svin_convert_result){
		.ticket = ++convert_next_ticket, .outcome = SVIN_CONVERT_PENDING};
	__atomic_store_n(&convert_phase, CONVERT_QUEUED, __ATOMIC_RELEASE);
	return convert_result.ticket;
}

bool svin_convert_cancel(uint64_t ticket) {
	unsigned phase = __atomic_load_n(&convert_phase, __ATOMIC_ACQUIRE);
	if(phase == CONVERT_IDLE || !ticket || ticket != convert_result.ticket)
		return false;
	for(;;) {
		if(phase == CONVERT_CANCEL_QUEUED || phase == CONVERT_CANCEL_RUNNING)
			return true;
		if(phase != CONVERT_QUEUED && phase != CONVERT_RUNNING)
			return false;
		unsigned desired = phase == CONVERT_QUEUED
			? CONVERT_CANCEL_QUEUED : CONVERT_CANCEL_RUNNING;
		if(__atomic_compare_exchange_n(&convert_phase, &phase, desired, false,
			__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
			return true;
	}
}

bool svin_convert_take(uint64_t* ticket, const char** path, bool* cancelled) {
	unsigned phase = __atomic_load_n(&convert_phase, __ATOMIC_ACQUIRE);
	for(;;) {
		if(phase != CONVERT_QUEUED && phase != CONVERT_CANCEL_QUEUED)
			return false;
		bool cancel = phase == CONVERT_CANCEL_QUEUED;
		unsigned desired = cancel ? CONVERT_CANCEL_RUNNING : CONVERT_RUNNING;
		if(__atomic_compare_exchange_n(&convert_phase, &phase, desired, false,
			__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			*ticket = convert_result.ticket;
			*path = convert_path;
			*cancelled = cancel;
			return true;
		}
	}
}

bool svin_convert_finish(uint64_t ticket, enum svin_convert_outcome outcome,
	bool may_have_changed) {
	unsigned phase = __atomic_load_n(&convert_phase, __ATOMIC_ACQUIRE);
	if(!ticket || phase == CONVERT_IDLE || ticket != convert_result.ticket
	   || outcome == SVIN_CONVERT_PENDING)
		return false;
	for(;;) {
		if(phase != CONVERT_RUNNING && phase != CONVERT_CANCEL_RUNNING)
			return false;
		bool cancel = phase == CONVERT_CANCEL_RUNNING;
		if(__atomic_compare_exchange_n(&convert_phase, &phase, CONVERT_FINISHING,
			false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			convert_result.outcome = cancel ? SVIN_CONVERT_CANCELLED : outcome;
			convert_result.may_have_changed = may_have_changed;
			__atomic_store_n(&convert_phase, CONVERT_FINISHED, __ATOMIC_RELEASE);
			return true;
		}
	}
}

bool svin_convert_read(uint64_t ticket, struct svin_convert_result* result) {
	if(__atomic_load_n(&convert_phase, __ATOMIC_ACQUIRE) != CONVERT_FINISHED
	   || !ticket || ticket != convert_result.ticket || !result)
		return false;
	*result = convert_result;
	return true;
}

bool svin_convert_release(uint64_t ticket) {
	if(__atomic_load_n(&convert_phase, __ATOMIC_ACQUIRE) != CONVERT_FINISHED
	   || !ticket || ticket != convert_result.ticket)
		return false;
	__atomic_store_n(&convert_phase, CONVERT_IDLE, __ATOMIC_RELEASE);
	return true;
}

/* Only after the local consumer is joined (or before any consumer exists). */
static void svin_convert_retire(void) {
	uint64_t ticket;
	const char* path;
	bool cancelled;
	if(svin_convert_take(&ticket, &path, &cancelled))
		(void)svin_convert_finish(ticket, SVIN_CONVERT_PROVIDER_STOPPED, false);
}

/* ===== R3 spawn-search mailbox (same phase machine as convert-only) ====== */
enum spawn_phase { SPAWN_IDLE, SPAWN_QUEUED, SPAWN_RUNNING, SPAWN_FINISHED };
static unsigned spawn_phase;
static uint64_t spawn_next_ticket;
static struct svin_spawn_result spawn_result;
static struct svin_spawn_request spawn_req;
static char spawn_dir[SVIN_SPAWN_DIR_MAX];
static unsigned spawn_cancel; /* set by the requester, read by the search */

bool svin_spawn_provider(void) {
	return svin_local_motion_on;
}

uint64_t svin_spawn_request(const char* dir, const struct svin_spawn_request* r) {
	if(!svin_local_motion_on || !dir || !dir[0] || !r
	   || strlen(dir) >= sizeof spawn_dir || spawn_next_ticket == UINT64_MAX
	   || __atomic_load_n(&spawn_phase, __ATOMIC_ACQUIRE) != SPAWN_IDLE)
		return 0;
	snprintf(spawn_dir, sizeof spawn_dir, "%s", dir);
	spawn_req = *r;
	spawn_result = (struct svin_spawn_result){
		.ticket = ++spawn_next_ticket, .outcome = SVIN_SPAWN_PENDING};
	__atomic_store_n(&spawn_cancel, 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&spawn_phase, SPAWN_QUEUED, __ATOMIC_RELEASE);
	return spawn_result.ticket;
}

bool svin_spawn_take(uint64_t* ticket, const char** dir,
					 struct svin_spawn_request* r) {
	unsigned phase = SPAWN_QUEUED;
	if(!__atomic_compare_exchange_n(&spawn_phase, &phase, SPAWN_RUNNING, false,
		__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return false;
	*ticket = spawn_result.ticket;
	*dir = spawn_dir;
	*r = spawn_req;
	return true;
}

bool svin_spawn_finish(uint64_t ticket, enum svin_spawn_outcome outcome,
					   const float world_spawn[3], const float player_spawn[3]) {
	if(!ticket || ticket != spawn_result.ticket || outcome == SVIN_SPAWN_PENDING
	   || __atomic_load_n(&spawn_phase, __ATOMIC_ACQUIRE) != SPAWN_RUNNING)
		return false;
	spawn_result.outcome = outcome;
	for(int i = 0; i < 3; i++) {
		spawn_result.world_spawn[i] = world_spawn ? world_spawn[i] : 0.0F;
		spawn_result.player_spawn[i] = player_spawn ? player_spawn[i] : 0.0F;
	}
	__atomic_store_n(&spawn_phase, SPAWN_FINISHED, __ATOMIC_RELEASE);
	return true;
}

bool svin_spawn_read(uint64_t ticket, struct svin_spawn_result* result) {
	if(__atomic_load_n(&spawn_phase, __ATOMIC_ACQUIRE) != SPAWN_FINISHED
	   || !ticket || ticket != spawn_result.ticket || !result)
		return false;
	*result = spawn_result;
	return true;
}

bool svin_spawn_release(uint64_t ticket) {
	if(__atomic_load_n(&spawn_phase, __ATOMIC_ACQUIRE) != SPAWN_FINISHED
	   || !ticket || ticket != spawn_result.ticket)
		return false;
	__atomic_store_n(&spawn_phase, SPAWN_IDLE, __ATOMIC_RELEASE);
	return true;
}

bool svin_spawn_cancel(uint64_t ticket) {
	unsigned phase = __atomic_load_n(&spawn_phase, __ATOMIC_ACQUIRE);
	if(!ticket || ticket != spawn_result.ticket
	   || (phase != SPAWN_QUEUED && phase != SPAWN_RUNNING))
		return false;
	__atomic_store_n(&spawn_cancel, 1u, __ATOMIC_RELEASE);
	return true;
}

bool svin_spawn_cancelled(void) {
	return __atomic_load_n(&spawn_cancel, __ATOMIC_ACQUIRE) != 0;
}

void svin_spawn_release_stale(void) {
	unsigned phase = SPAWN_FINISHED;
	(void)__atomic_compare_exchange_n(&spawn_phase, &phase, SPAWN_IDLE, false,
		__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

static void svin_spawn_retire(void) {
	uint64_t ticket;
	const char* dir;
	struct svin_spawn_request r;
	if(svin_spawn_take(&ticket, &dir, &r))
		(void)svin_spawn_finish(ticket, SVIN_SPAWN_PROVIDER_STOPPED, NULL, NULL);
}
static size_t svin_pending_control_count[2] = {0};
/* which pending entries hold a control-reserve slot (others are counted in
 * their ordinary class); 203 elements (Wii/PC: 203 bytes). The mutex,
 * credit counters and body fields are inventoried in docs/deviations.md. */
static bool svin_pending_in_control[SVIN_PENDING_SIZE];

/* ===== V6 F1 round 3: movement credits (see server_interface.h) ========== */
static struct thread_mutex svin_credit_mx;
static bool svin_credit_mx_ready = false;
static uint32_t svin_credit_epoch = 1;      /* local session; under the lock */
static uint64_t svin_credit_gen[2];         /* owner lifetime; under the lock */
static uint32_t svin_credit_out[2];         /* outstanding; under the lock */
static uint32_t svin_credit_unpub[2];       /* reserved, not yet published (main) */
static uint64_t svin_host_credit_gen = 0;   /* main thread */
static uint32_t svin_credit_stale = 0;      /* under the lock */
/* the credited PLAYER_POS currently being dispatched (server thread) */
static const struct server_rpc* svin_cur_credit_rpc;
static bool svin_cur_credit_taken;
#ifdef CAVEWII_SERVER_LOCAL_TEST
void (*svin_test_credit_publish_hook)(struct server_rpc_source,
	const struct server_rpc*);
#endif

static bool svin_is_lifecycle(enum server_rpc_type type) {
	return type == SRPC_WINDOW_CLOSE || type == SRPC_UNLOAD_WORLD
		|| type == SRPC_LOAD_WORLD;
}

static bool svin_is_input_ack(enum server_rpc_type type) {
	return type == SRPC_INPUT_READY;
}

static bool svin_is_stream_recover(enum server_rpc_type type) {
	return type == SRPC_STREAM_RECOVER;
}

static bool svin_is_player_state(enum server_rpc_type type) {
	return type == SRPC_PLAYER_STATE;
}

static bool svin_is_minecart_control(enum server_rpc_type type) {
	return type == SRPC_MINECART_CONTROL;
}

static bool svin_is_ride_intent(enum server_rpc_type type) {
	return type == SRPC_MINECART_RIDE_INTENT;
}

static bool svin_is_split_set(enum server_rpc_type type) {
	return type == SRPC_SPLIT_SET;
}

/* Only ever true for an entry queued in local-motion mode (remote mode drops
 * a full-inbox PLAYER_POS before it can be queued). */
static bool svin_is_motion(enum server_rpc_type type) {
	return type == SRPC_PLAYER_POS;
}

static bool svin_is_control(enum server_rpc_type type) {
	return type == SRPC_TOGGLE_PAUSE || type == SRPC_LOCAL_GUEST;
}

/* RFC-1982-style comparison for uint32 protocol counters. A half-range delta
 * is deliberately ambiguous and therefore not newer. */
static bool svin_serial_newer(uint32_t candidate, uint32_t current) {
	uint32_t delta = candidate - current;
	return delta != 0 && delta < UINT32_C(0x80000000);
}

size_t svin_pending_count(void) {
	return svin_pending_len;
}

bool svin_input_ready_pending(uint32_t epoch) {
	if(epoch == 0)
		return false;
	for(size_t i = 0; i < svin_pending_len; i++) {
		size_t idx = (svin_pending_head + i) % SVIN_PENDING_SIZE;
		if(svin_pending[idx].type == SRPC_INPUT_READY
		   && svin_pending[idx].payload.input_ready.epoch == epoch)
			return true;
	}
	return false;
}

bool svin_init() {
	if(!tchannel_init(&svin_inbox, RPC_INBOX_SIZE))
		return false;
	if(!tchannel_init(&svin_empty_msg, RPC_INBOX_SIZE)) {
		tchannel_close(&svin_inbox);
		return false;
	}

	// Seeds exactly RPC_INBOX_SIZE tokens into a fresh channel of that capacity
	// (k-th send sees count k < cap), before the server thread exists.
	for(int k = 0; k < RPC_INBOX_SIZE; k++)
		if(!tchannel_send(&svin_empty_msg, rpc_msg + k, true))
			return false;

	svin_pending_head = 0;
	svin_held_n = 0; /* v5: a fresh channel pair owns every token again */
	__atomic_store_n(&svin_active_rpc, 0, __ATOMIC_RELAXED);
	svin_pending_len = 0;
	svin_pending_lifecycle_count[0] = svin_pending_lifecycle_count[1] = 0;
	svin_pending_input_ack_count = 0;
	svin_pending_stream_recover_count = 0;
	svin_pending_player_state_count[0] = svin_pending_player_state_count[1] = 0;
	svin_pending_action_count[0] = svin_pending_action_count[1] = 0;
	svin_pending_minecart_control_count = 0;
	svin_pending_ride_intent_count = 0;
	svin_pending_split_set_count = 0;
	svin_pending_motion_count[0] = svin_pending_motion_count[1] = 0;
	svin_pending_control_count[0] = svin_pending_control_count[1] = 0;
	for(size_t k = 0; k < SVIN_PENDING_SIZE; k++)
		svin_pending_in_control[k] = false;
	svin_motion_refused = 0;
	if(!svin_credit_mx_ready) {
		if(!thread_mutex_init(&svin_credit_mx))
			return false;
		svin_credit_mx_ready = true;
	}
	thread_mutex_lock(&svin_credit_mx);
	svin_credit_epoch++;
	svin_credit_out[0] = svin_credit_out[1] = 0;
	svin_credit_gen[0] = svin_credit_gen[1] = 0;
	svin_credit_stale = 0;
	thread_mutex_unlock(&svin_credit_mx);
	svin_credit_unpub[0] = svin_credit_unpub[1] = 0;
	svin_host_credit_gen = 0;
	svin_cur_credit_rpc = NULL;
	svin_local_motion_on = false; /* the local provider's start arms it */
	for(size_t k = 0; k < RPC_INBOX_SIZE; k++)
		rpc_ctx[k] = 0;
	svin_cur_ctx = 0;
	svin_cancelled = 0;
	svin_dropped = 0;
	__atomic_store_n(&svin_split_accept, false, __ATOMIC_RELAXED);
	return true;
}

void svin_process_owned_messages(void (*process)(struct server_rpc_source,
	struct server_rpc*, void*), void* user, bool block) {
	assert(process);

	void* call;
	while(tchannel_receive(&svin_inbox, &call, block)) {
		struct server_rpc* rpc = call;
		struct server_rpc_source source = rpc_source[rpc - rpc_msg];
		uint32_t marker = ((uint32_t)source.slot << 16)
			| ((uint32_t)rpc->type + 1u);
		__atomic_store_n(&svin_active_rpc, marker, __ATOMIC_RELAXED);
		process(source, rpc, user);
		// Never full: inbox + empty_msg + in-hand always sum to the
		// RPC_INBOX_SIZE rpc_msg tokens (== each channel's capacity), and this
		// thread holds the token it just drained from the inbox.
		tchannel_send(&svin_empty_msg, call, true);
		__atomic_store_n(&svin_active_rpc, 0, __ATOMIC_RELAXED);
	}
}

/* v5 (ASTRA-V4-REVIEW (2) queue ruling): Beta NetworkManager.processRead-
 * Packets (NetworkManager.java:191-209) handles at most 101 packets of one
 * connection per call (`var1 = 100; while(!empty && var1-- >= 0)`) and leaves
 * the rest in readPackets for the next tick -- deferred, never discarded.
 * A message the admit callback declines is HELD here, still owning its
 * rpc_msg token, and every later message of the SAME source in this drain is
 * held behind it (per-connection FIFO); other sources continue. The next
 * drain offers the held messages first, in order, then the inbox. Because a
 * held message keeps its token, the ring can never hold more than
 * RPC_INBOX_SIZE entries (inbox + empty pool + held == the token count), and
 * the producer is back-pressured by exactly the held count: lossless and
 * bounded. svin_process_owned_messages / svin_process_messages (the legacy
 * and remote providers) never hold anything and are unchanged. */
size_t svin_held_count(void) {
	return svin_held_n;
}

static void svin_dispatch_owned(void (*process)(struct server_rpc_source,
	struct server_rpc*, void*), void* user, struct server_rpc* rpc) {
	struct server_rpc_source source = rpc_source[rpc - rpc_msg];
	uint32_t marker = ((uint32_t)source.slot << 16)
		| ((uint32_t)rpc->type + 1u);
	__atomic_store_n(&svin_active_rpc, marker, __ATOMIC_RELAXED);
	svin_cur_ctx = rpc_ctx[rpc - rpc_msg];
	const bool credited = rpc->type == SRPC_PLAYER_POS
		&& rpc->payload.player_pos.credited;
	svin_cur_credit_rpc = credited ? rpc : NULL;
	svin_cur_credit_taken = false;
	process(source, rpc, user);
	/* a credited sample the handler did not turn into an actor-queue event
	 * is terminally settled here -- not when its token is freed earlier */
	if(credited && !svin_cur_credit_taken)
		svin_credit_release(rpc);
	svin_cur_credit_rpc = NULL;
	svin_cur_ctx = 0;
	rpc_ctx[rpc - rpc_msg] = 0;
	tchannel_send(&svin_empty_msg, rpc, true);
	__atomic_store_n(&svin_active_rpc, 0, __ATOMIC_RELAXED);
}

/* The v5 entry point (bool admit, no progress route) is the v6 drain with a
 * two-valued admission; kept for callers that need nothing more. */
static bool (*svin_v5_admit)(struct server_rpc_source, const struct server_rpc*,
	void*);
static enum svin_admit_result svin_v5_adapt(struct server_rpc_source source,
	const struct server_rpc* call, void* user) {
	return svin_v5_admit(source, call, user) ? SVIN_ADMIT : SVIN_DEFER;
}
void svin_process_owned_messages_admitted(
	void (*process)(struct server_rpc_source, struct server_rpc*, void*),
	bool (*admit)(struct server_rpc_source, const struct server_rpc*, void*),
	void* user) {
	assert(process && admit);
	svin_v5_admit = admit;
	const struct svin_drain_ops ops = {.process = process,
									   .admit = svin_v5_adapt};
	svin_process_owned_messages_ops(&ops, user);
}

/* ===== V6 F1 (ASTRA-V5-REVIEW R2): the progress route ====================
 * A source whose FIFO is held because a message needs server storage that
 * only a later update can free (DEFER_STORAGE: a movement sample behind a
 * full pause-held motion queue) would otherwise hold its own pause/resume/
 * leave behind that sample forever -- the R2 deadlock. Here such a source's
 * first later CONTROL runs ahead of the held messages it overtakes:
 *   1. each overtaken message without a stamp is stamped with the state it
 *      would have been processed under in FIFO order (ops->context, read
 *      before the control runs); an already-stamped one keeps the earlier
 *      (older, correct) stamp;
 *   2. the control is dispatched (it spends the update budget via admit);
 *   3. if the control changed the source's lifetime (ops->lifetime before !=
 *      after: an unload or a guest leave that took effect), the overtaken
 *      messages belong to an ended lifetime and are CANCELLED -- never
 *      processed, ops->cancel told, tokens returned. Messages after the
 *      control (a following LOAD_WORLD) are untouched.
 * Repeat for the next control of that source. Non-control messages keep
 * their mutual order; controls keep theirs. Only DEFER_STORAGE opens the
 * route: a budget DEFER resumes next update on its own. */
static void svin_held_remove(size_t index) {
	assert(index < svin_held_n);
	for(size_t k = index + 1; k < svin_held_n; k++)
		svin_held[k - 1] = svin_held[k];
	svin_held_n--;
}
static void svin_progress_controls(const struct svin_drain_ops* ops,
	void* user, const uint8_t* blocked) {
	for(unsigned slot = 0; slot < 256; slot++) {
		if(blocked[slot] != 2)
			continue;
		for(;;) {
			size_t j = 0;
			bool found = false;
			for(; j < svin_held_n; j++) {
				struct server_rpc* rpc = svin_held[j];
				struct server_rpc_source src = rpc_source[rpc - rpc_msg];
				if(src.slot == slot && ops->control(src, rpc, user)) {
					found = true;
					break;
				}
			}
			if(!found)
				break;
			struct server_rpc* control = svin_held[j];
			struct server_rpc_source src = rpc_source[control - rpc_msg];
			if(ops->admit(src, control, user) != SVIN_ADMIT)
				break; /* budget spent: the next drain continues */
			const uint32_t ctx = ops->context ? ops->context(src, user) : 0;
			assert(!ops->context || ctx != 0);
			for(size_t k = 0; k < j; k++) {
				struct server_rpc* rpc = svin_held[k];
				if(rpc_source[rpc - rpc_msg].slot == slot && !rpc_ctx[rpc - rpc_msg])
					rpc_ctx[rpc - rpc_msg] = ctx;
			}
			const uint64_t life0 = ops->lifetime ? ops->lifetime(src, user) : 0;
			svin_held_remove(j);
			svin_dispatch_owned(ops->process, user, control);
			const uint64_t life1 = ops->lifetime ? ops->lifetime(src, user) : 0;
			if(life0 == life1)
				continue;
			/* the overtaken input of the ended lifetime: the first j entries
			 * (the control sat at j) of this slot */
			size_t kept = 0;
			for(size_t k = 0; k < svin_held_n; k++) {
				struct server_rpc* rpc = svin_held[k];
				struct server_rpc_source rs = rpc_source[rpc - rpc_msg];
				if(k < j && rs.slot == slot) {
					if(ops->cancel)
						ops->cancel(rs, rpc, user);
					svin_credit_release(rpc); /* lifetime cancellation */
					rpc_ctx[rpc - rpc_msg] = 0;
					tchannel_send(&svin_empty_msg, rpc, true);
					svin_cancelled++;
					continue;
				}
				svin_held[kept++] = rpc;
			}
			svin_held_n = kept;
		}
	}
}

void svin_process_owned_messages_ops(const struct svin_drain_ops* ops,
	void* user) {
	assert(ops && ops->process && ops->admit);
	uint8_t blocked[256] = {0}; /* 0 open, 1 DEFER, 2 DEFER_STORAGE */
	/* 1. the held backlog, oldest first */
	size_t kept = 0;
	for(size_t i = 0; i < svin_held_n; i++) {
		struct server_rpc* rpc = svin_held[i];
		struct server_rpc_source source = rpc_source[rpc - rpc_msg];
		if(!blocked[source.slot]) {
			enum svin_admit_result r = ops->admit(source, rpc, user);
			if(r == SVIN_ADMIT) {
				svin_dispatch_owned(ops->process, user, rpc);
				continue;
			}
			blocked[source.slot] = r == SVIN_DEFER_STORAGE ? 2 : 1;
		}
		svin_held[kept++] = rpc;
	}
	svin_held_n = kept;
	/* 2. the inbox (never blocking: a held token must not wait for itself) */
	void* call;
	while(tchannel_receive(&svin_inbox, &call, false)) {
		struct server_rpc* rpc = call;
		struct server_rpc_source source = rpc_source[rpc - rpc_msg];
		if(!blocked[source.slot]) {
			enum svin_admit_result r = ops->admit(source, rpc, user);
			if(r == SVIN_ADMIT) {
				svin_dispatch_owned(ops->process, user, rpc);
				continue;
			}
			blocked[source.slot] = r == SVIN_DEFER_STORAGE ? 2 : 1;
		}
		assert(svin_held_n < RPC_INBOX_SIZE);
		svin_held[svin_held_n++] = rpc;
	}
	/* 3. the progress route of every storage-held source */
	if(ops->control)
		svin_progress_controls(ops, user, blocked);
}

uint32_t svin_dispatch_context(void) {
	return svin_cur_ctx;
}
uint32_t svin_cancelled_count(void) {
	return svin_cancelled;
}

/* ===== V6 F1 round 3: credit API ===================================== */
static uint64_t svin_credit_owner_gen(struct server_rpc_source source) {
	return source.slot == 0 ? svin_host_credit_gen : source.generation;
}
bool svin_credit_reserve(struct server_rpc_source source) {
	if(!svin_local_motion_on)
		return true; /* remote/legacy: no accounting */
	if(source.slot > 1)
		return false;
	const unsigned o = source.slot;
	/* physical publication storage: one pending motion slot per reservation
	 * (direct sends need none, but a reservation must never depend on a
	 * token being free later) */
	if(svin_pending_motion_count[o] + svin_credit_unpub[o]
		   >= SVIN_LOCAL_MOTION_RESERVE
	   || svin_pending_len + svin_credit_unpub[0] + svin_credit_unpub[1]
		   >= SVIN_PENDING_SIZE)
		return false;
	const uint64_t gen = svin_credit_owner_gen(source);
	bool ok;
	thread_mutex_lock(&svin_credit_mx);
	if(svin_credit_gen[o] != gen) { /* a new owner lifetime: fresh count */
		svin_credit_gen[o] = gen;
		svin_credit_out[o] = 0;
	}
	ok = svin_credit_out[o] < SVIN_MOTION_CREDITS;
	if(ok)
		svin_credit_out[o]++;
	thread_mutex_unlock(&svin_credit_mx);
	if(ok)
		svin_credit_unpub[o]++;
	return ok;
}
void svin_credit_publish(struct server_rpc_source source, struct server_rpc* call) {
	assert(call && call->type == SRPC_PLAYER_POS);
	if(!svin_local_motion_on) {
		call->payload.player_pos.credited = false;
		(void)svin_rpc_send_from(source, call); /* the remote contract */
		return;
	}
	assert(source.slot < 2 && svin_credit_unpub[source.slot] > 0);
	call->payload.player_pos.credited = true;
	thread_mutex_lock(&svin_credit_mx);
	call->payload.player_pos.credit_epoch = svin_credit_epoch;
	thread_mutex_unlock(&svin_credit_mx);
	call->payload.player_pos.credit_gen = svin_credit_owner_gen(source);
	call->payload.player_pos.credit_slot = source.slot;
	svin_credit_unpub[source.slot]--;
	const bool sent = svin_rpc_send_from(source, call);
	/* the reservation guaranteed storage: a refusal here is a bug */
	assert(sent);
#ifdef CAVEWII_SERVER_LOCAL_TEST
	if(sent && svin_test_credit_publish_hook)
		svin_test_credit_publish_hook(source, call);
#endif
	if(!sent)
		svin_credit_release(call);
}
void svin_credit_abandon(struct server_rpc_source source) {
	if(!svin_local_motion_on || source.slot > 1)
		return;
	assert(svin_credit_unpub[source.slot] > 0);
	svin_credit_unpub[source.slot]--;
	thread_mutex_lock(&svin_credit_mx);
	if(svin_credit_out[source.slot] > 0)
		svin_credit_out[source.slot]--;
	thread_mutex_unlock(&svin_credit_mx);
}
void svin_credit_release_tag(unsigned slot, uint32_t epoch, uint64_t gen,
	unsigned n) {
	if(slot > 1 || n == 0 || !svin_credit_mx_ready)
		return;
	thread_mutex_lock(&svin_credit_mx);
	if(epoch != svin_credit_epoch || gen != svin_credit_gen[slot]) {
		svin_credit_stale += n; /* an ended lifetime: never a current credit */
	} else {
		assert(svin_credit_out[slot] >= n); /* no double release */
		svin_credit_out[slot] -= n <= svin_credit_out[slot] ? n
								   : svin_credit_out[slot];
	}
	thread_mutex_unlock(&svin_credit_mx);
}
void svin_credit_release(const struct server_rpc* call) {
	if(!call || call->type != SRPC_PLAYER_POS || !call->payload.player_pos.credited)
		return;
	svin_credit_release_tag(call->payload.player_pos.credit_slot,
		call->payload.player_pos.credit_epoch,
		call->payload.player_pos.credit_gen, 1);
}
bool svin_credit_old_tag_stale(unsigned slot, uint32_t epoch, uint64_t gen) {
	if(slot > 1 || !svin_credit_mx_ready)
		return false;
	thread_mutex_lock(&svin_credit_mx);
	const bool stale = epoch != svin_credit_epoch || gen != svin_credit_gen[slot];
	thread_mutex_unlock(&svin_credit_mx);
	return stale;
}
uint32_t svin_credit_outstanding(unsigned slot) {
	if(slot > 1 || !svin_credit_mx_ready)
		return 0;
	thread_mutex_lock(&svin_credit_mx);
	uint32_t n = svin_credit_out[slot];
	thread_mutex_unlock(&svin_credit_mx);
	return n;
}
uint32_t svin_credit_stale_releases(void) {
	thread_mutex_lock(&svin_credit_mx);
	uint32_t n = svin_credit_stale;
	thread_mutex_unlock(&svin_credit_mx);
	return n;
}
bool svin_dispatch_credit_take(uint32_t* epoch, uint64_t* gen) {
	if(!svin_cur_credit_rpc || svin_cur_credit_taken)
		return false;
	svin_cur_credit_taken = true;
	if(epoch)
		*epoch = svin_cur_credit_rpc->payload.player_pos.credit_epoch;
	if(gen)
		*gen = svin_cur_credit_rpc->payload.player_pos.credit_gen;
	return true;
}
void svin_dispatch_credit_untake(void) {
	if(svin_cur_credit_rpc)
		svin_cur_credit_taken = false;
}
void svin_credit_host_lifetime_bump(void) {
	svin_host_credit_gen++;
}

void svin_stamp_received(int except_slot, uint32_t ctx) {
	assert(ctx != 0);
	for(size_t k = 0; k < svin_held_n; k++) {
		struct server_rpc* rpc = svin_held[k];
		if((int)rpc_source[rpc - rpc_msg].slot != except_slot
		   && !rpc_ctx[rpc - rpc_msg])
			rpc_ctx[rpc - rpc_msg] = ctx;
	}
}

void svin_set_local_motion(bool enabled) {
	if(!enabled) {
		svin_convert_retire();
		svin_spawn_retire();
	}
	if(enabled && !svin_local_motion_on && svin_credit_mx_ready) {
		/* a new local session: a new credit identity epoch */
		thread_mutex_lock(&svin_credit_mx);
		svin_credit_epoch++;
		svin_credit_out[0] = svin_credit_out[1] = 0;
		thread_mutex_unlock(&svin_credit_mx);
		svin_credit_unpub[0] = svin_credit_unpub[1] = 0;
	}
	svin_local_motion_on = enabled;
}
bool svin_local_motion(void) {
	return svin_local_motion_on;
}
bool svin_motion_room(struct server_rpc_source source) {
	if(!svin_local_motion_on)
		return true; /* remote/legacy: unchanged drop semantics */
	if(source.slot > 1)
		return false;
	return svin_pending_motion_count[source.slot] < SVIN_LOCAL_MOTION_RESERVE
		&& svin_pending_len < SVIN_PENDING_SIZE;
}
size_t svin_motion_pending_count(unsigned slot) {
	return slot < 2 ? svin_pending_motion_count[slot] : 0;
}
uint32_t svin_motion_refused_count(void) {
	return svin_motion_refused;
}

size_t svin_retire_local(void) {
	svin_convert_retire();
	svin_spawn_retire();
	size_t retired = 0;
	for(size_t k = 0; k < svin_held_n; k++) {
		struct server_rpc* rpc = svin_held[k];
		if(rpc->type == SRPC_LOAD_WORLD)
			string_clear(rpc->payload.load_world.name);
		svin_credit_release(rpc);
		rpc_ctx[rpc - rpc_msg] = 0;
		tchannel_send(&svin_empty_msg, rpc, true);
		svin_cancelled++;
		retired++;
	}
	svin_held_n = 0;
	/* compact the pending ring in place, keeping every non-motion entry in
	 * its relative order (the svin_rpc_drop_owner technique) */
	size_t kept = 0;
	for(size_t i = 0; i < svin_pending_len; i++) {
		size_t idx = (svin_pending_head + i) % SVIN_PENDING_SIZE;
		if(svin_is_motion(svin_pending[idx].type)) {
			unsigned slot = svin_pending_source[idx].slot;
			assert(svin_pending_motion_count[slot] > 0);
			svin_pending_motion_count[slot]--;
			svin_credit_release(&svin_pending[idx]);
			retired++;
			continue;
		}
		size_t wp = (svin_pending_head + kept) % SVIN_PENDING_SIZE;
		if(wp != idx) {
			svin_pending[wp] = svin_pending[idx];
			svin_pending_source[wp] = svin_pending_source[idx];
			svin_pending_in_control[wp] = svin_pending_in_control[idx];
			svin_pending_in_control[idx] = false;
		}
		kept++;
	}
	svin_pending_len = kept;
	assert(svin_pending_motion_count[0] == 0 && svin_pending_motion_count[1] == 0);
	svin_local_motion_on = false;
	/* every credit of the ended session is gone with it */
	thread_mutex_lock(&svin_credit_mx);
	svin_credit_epoch++;
	svin_credit_out[0] = svin_credit_out[1] = 0;
	thread_mutex_unlock(&svin_credit_mx);
	svin_credit_unpub[0] = svin_credit_unpub[1] = 0;
	return retired;
}

/* Legacy/remote providers consume primary requests only. A guest can never
 * fall through to a handler whose player identity is implicit. */
struct svin_primary_dispatch {
	void (*process)(struct server_rpc*, void*);
	void* user;
};
static void svin_process_primary(struct server_rpc_source source,
	struct server_rpc* call, void* user) {
	struct svin_primary_dispatch* dispatch = user;
	if(svin_source_equal(source, (struct server_rpc_source){0}))
		dispatch->process(call, dispatch->user);
}
void svin_process_messages(void (*process)(struct server_rpc*, void*),
	void* user, bool block) {
	assert(process);
	struct svin_primary_dispatch dispatch = {process, user};
	svin_process_owned_messages(svin_process_primary, &dispatch, block);
}

// Try to place one message in the inbox without blocking. NON-BLOCKING is load-bearing
// (deadlock fix): blocking here was the client half of a two-way bounded-buffer DEADLOCK
// -- client blocked on the full server inbox while the server blocked on the full client
// chunk pool, neither draining -> a hard FREEZE seconds into loading a non-superflat
// world (perf "binding constraint #5"). The client render thread MUST stay live every
// frame so clin_update keeps draining clin_inbox and freeing the server's chunk slots.
static bool svin_try_send_now(struct server_rpc_source source,
	const struct server_rpc* call) {
	struct server_rpc* empty;
	if(!tchannel_receive(&svin_empty_msg, (void**)&empty, false))
		return false;
	*empty = *call;
	rpc_source[empty - rpc_msg] = source;
	rpc_ctx[empty - rpc_msg] = 0;
	// We hold an empty slot, so inbox has room (empty+inbox == RPC_INBOX_SIZE) -> never blocks.
	tchannel_send(&svin_inbox, empty, true);
	return true;
}

void svin_rpc_flush_pending(void) {
	while(svin_pending_len > 0) {
		if(!svin_try_send_now(svin_pending_source[svin_pending_head],
			&svin_pending[svin_pending_head]))
			return; // inbox still full; retry next frame / next send
		if(svin_is_lifecycle(svin_pending[svin_pending_head].type)) {
			unsigned slot = svin_pending_source[svin_pending_head].slot;
			assert(svin_pending_lifecycle_count[slot] > 0);
			svin_pending_lifecycle_count[slot]--;
		} else if(svin_is_input_ack(
				  svin_pending[svin_pending_head].type)) {
			assert(svin_pending_input_ack_count > 0);
			svin_pending_input_ack_count--;
		} else if(svin_is_stream_recover(
				  svin_pending[svin_pending_head].type)) {
			assert(svin_pending_stream_recover_count > 0);
			svin_pending_stream_recover_count--;
		} else if(svin_is_player_state(
				  svin_pending[svin_pending_head].type)) {
			unsigned slot = svin_pending_source[svin_pending_head].slot;
			assert(svin_pending_player_state_count[slot] > 0);
			svin_pending_player_state_count[slot]--;
		} else if(svin_is_minecart_control(
				  svin_pending[svin_pending_head].type)) {
			assert(svin_pending_minecart_control_count > 0);
			svin_pending_minecart_control_count--;
		} else if(svin_is_ride_intent(
				  svin_pending[svin_pending_head].type)) {
			assert(svin_pending_ride_intent_count > 0);
			svin_pending_ride_intent_count--;
		} else if(svin_is_split_set(
				  svin_pending[svin_pending_head].type)) {
			assert(svin_pending_split_set_count > 0);
			svin_pending_split_set_count--;
		} else if(svin_is_motion(svin_pending[svin_pending_head].type)) {
			unsigned slot = svin_pending_source[svin_pending_head].slot;
			assert(svin_pending_motion_count[slot] > 0);
			svin_pending_motion_count[slot]--;
		} else if(svin_pending_in_control[svin_pending_head]) {
			unsigned slot = svin_pending_source[svin_pending_head].slot;
			assert(svin_pending_control_count[slot] > 0);
			svin_pending_control_count[slot]--;
			svin_pending_in_control[svin_pending_head] = false;
		} else {
			/* Ordinary gameplay action: release the owner's reservation on
			 * DISPATCH -- this is the only path that hands an entry to the
			 * inbox. A rejected action never took one, and teardown releases
			 * whatever is left through svin_rpc_drop_owner / svin_init. */
			unsigned slot = svin_pending_source[svin_pending_head].slot;
			assert(svin_pending_action_count[slot] > 0);
			svin_pending_action_count[slot]--;
		}
		svin_pending_head = (svin_pending_head + 1) % SVIN_PENDING_SIZE;
		svin_pending_len--;
	}
}

bool svin_rpc_send(struct server_rpc* call) {
	return svin_rpc_send_from((struct server_rpc_source){0}, call);
}

bool svin_rpc_send_from(struct server_rpc_source source, struct server_rpc* call) {
	assert(call);
	if(source.slot > 1 || (source.slot == 0 && source.generation != 0)
	   || (source.slot == 1 && (source.generation == 0
		   || (call->type != SRPC_WINDOW_CLICK && call->type != SRPC_WINDOW_DROP
			   && call->type != SRPC_WINDOW_CLOSE && call->type != SRPC_WINDOW_TRANSACTION
			   && call->type != SRPC_WINDOW_SORT_ADMIT && call->type != SRPC_SIGN_SET
			   && call->type != SRPC_RECIPE_CRAFT
			   && call->type != SRPC_LOCAL_GUEST
			   && call->type != SRPC_RESPAWN
			   /* Experimental local split-screen: an ADMITTED guest body
			    * streams its own movement. Gated on svin_split_accept, so
			    * with the switch OFF this whole class is rejected here --
			    * no inbox token, no dispatch, exactly as before the lane.
			    * With it ON the server still refuses the message unless
			    * that generation actually holds a gameplay body
			    * (server_local_guest_position), so a window-only guest
			    * sending this is inert -- and it can never reach the
			    * host's actor, which only slot 0 addresses. */
			   /* S1-a1: the seven per-actor GAMEPLAY verbs join the same
			    * svin_split_accept class as the movement stream -- with the
			    * switch OFF the whole class is rejected here (no inbox token,
			    * no dispatch, exactly as before the lane), and with it ON the
			    * server still refuses each message unless that generation
			    * actually holds an admitted gameplay body and the acting
			    * actor clears its own barriers
			    * (server_local_guest_action_allowed). PLAYER_STATE is NOT in
			    * this list AT S1-a1; S1-a2 made that latch per actor
			    * (struct server_player.reject_pos_this_update) and added
			    * PLAYER_STATE as the eighth verb, so it is in the class now. */
			   && (!__atomic_load_n(&svin_split_accept, __ATOMIC_RELAXED)
				   || (call->type != SRPC_PLAYER_POS
					   && call->type != SRPC_HOTBAR_SLOT
					   && call->type != SRPC_DROP_ITEM
					   && call->type != SRPC_CREATIVE_GIVE
					   && call->type != SRPC_USE_ITEM
					   && call->type != SRPC_BLOCK_DIG
					   && call->type != SRPC_BLOCK_PLACE
					   && call->type != SRPC_ENTITY_ATTACK
					   && call->type != SRPC_ENTITY_USE
					   && call->type != SRPC_PLAYER_STATE)))))) {
		/* host_vehicle_smoke_8m: a slot-1 RPC refused at this closed mailbox
		 * gate, observed on the calling (main) thread. Dormant: one load. */
		void (*observe)(const struct server_rpc*, uint8_t, uint64_t, bool)
			= __atomic_load_n(&svin_vsp_ingress_observer, __ATOMIC_RELAXED);
		if(observe)
			observe(call, source.slot, source.generation, false);
		return false;
	}
	{
		/* ...and one ADMITTED through it (contract ingress site = this send,
		 * status sent). Observe-only; dormant: one load. */
		void (*observe)(const struct server_rpc*, uint8_t, uint64_t, bool)
			= __atomic_load_n(&svin_vsp_ingress_observer, __ATOMIC_RELAXED);
		if(observe && source.slot == 1)
			observe(call, source.slot, source.generation, true);
	}

	// Anything already pending goes first -- FIFO order across the queue boundary
	// (a dig's finished=true must not overtake its finished=false).
	svin_rpc_flush_pending();

	if(call->type == SRPC_PLAYER_POS && svin_local_motion_on
	   && !call->payload.player_pos.credited) {
		svin_motion_refused++;
		svin_dropped++;
		return false; /* no unchecked direct-send escape hatch (ruling 6) */
	}
	if(svin_pending_len == 0 && svin_try_send_now(source, call))
		return true;

	// Inbox full (server behind on a stream flood / lag spike). Two classes:
	// - SRPC_PLAYER_POS is periodic + idempotent (the next 50ms tick re-sends a
	//   fresher one) -> dropping it is strictly better than queueing a stale one.
	// - Everything else is a player ACTION or a lifecycle message; Beta's TCP
	//   channel delays these under load but never loses them -> queue + retry.
	if(call->type == SRPC_PLAYER_POS && !svin_local_motion_on) {
		svin_dropped++;
		return false;
	}
	/* local mode: only a credited publication may carry movement (no
	 * unchecked direct-send escape hatch, ruling item 6) */
	if(call->type == SRPC_PLAYER_POS && !call->payload.player_pos.credited) {
		svin_motion_refused++;
		svin_dropped++;
		return false;
	}

	// Coalescing an identical ownership release is safe and preserves the FIFO
	// position of the first request.
	if(call->type == SRPC_WINDOW_CLOSE) {
		for(size_t i = 0; i < svin_pending_len; i++) {
			size_t idx = (svin_pending_head + i) % SVIN_PENDING_SIZE;
			if(!svin_source_equal(source, svin_pending_source[idx])) continue;
			if(svin_pending[idx].type == SRPC_WINDOW_CLOSE
			   && svin_pending[idx].payload.window_close.window
				  == call->payload.window_close.window)
				return true;
		}
	}
	/* Retried processing of the same tagged RETURN is idempotent. Preserve the
	 * first response's FIFO position instead of consuming the one reserved slot
	 * twice. If an inconsistent success/failure pair is ever submitted for the
	 * same epoch, failure dominates: releasing input without the center section
	 * would be unsafe, while an unnecessary restream is recoverable. */
	if(call->type == SRPC_INPUT_READY) {
		for(size_t i = 0; i < svin_pending_len; i++) {
			size_t idx = (svin_pending_head + i) % SVIN_PENDING_SIZE;
			if(!svin_source_equal(source, svin_pending_source[idx])) continue;
			if(svin_pending[idx].type == SRPC_INPUT_READY
			   && svin_pending[idx].payload.input_ready.epoch
				  == call->payload.input_ready.epoch) {
				svin_pending[idx].payload.input_ready.retry
					= svin_pending[idx].payload.input_ready.retry
					  || call->payload.input_ready.retry;
				return true;
			}
		}
	}
	/* A retry of the same stable-generation recovery is idempotent. Keep the
	 * first request's FIFO position and its dedicated capacity. */
	if(call->type == SRPC_STREAM_RECOVER) {
		for(size_t i = 0; i < svin_pending_len; i++) {
			size_t idx = (svin_pending_head + i) % SVIN_PENDING_SIZE;
			if(!svin_source_equal(source, svin_pending_source[idx])) continue;
			if(svin_pending[idx].type == SRPC_STREAM_RECOVER
			   && svin_pending[idx].payload.stream_recover.epoch
				  == call->payload.stream_recover.epoch)
				return true;
		}
	}
	/* Movement state is an idempotent snapshot. If a newer edge arrives while
	 * the dedicated slot is pending, replace its payload at the original FIFO
	 * position: no later position sample can overtake it, and the server receives
	 * the newest state instead of a stale transition train. */
	if(call->type == SRPC_PLAYER_STATE) {
		for(size_t i = 0; i < svin_pending_len; i++) {
			size_t idx = (svin_pending_head + i) % SVIN_PENDING_SIZE;
			if(!svin_source_equal(source, svin_pending_source[idx])) continue;
			if(svin_pending[idx].type == SRPC_PLAYER_STATE) {
				svin_pending[idx] = *call;
				return true;
			}
		}
	}
	/* Minecart input is cumulative, not a delta. Replace a pending snapshot for
	 * the SAME cart and ride epoch at its original FIFO position, but only when
	 * the serial is newer. Equal/older retries are already represented by the
	 * queued newest snapshot. Never replace across epochs: doing so could move a
	 * remount's input ahead of the dismount/remount actions that establish it. */
	if(call->type == SRPC_MINECART_CONTROL) {
		for(size_t i = 0; i < svin_pending_len; i++) {
			size_t idx = (svin_pending_head + i) % SVIN_PENDING_SIZE;
			if(!svin_source_equal(source, svin_pending_source[idx])) continue;
			const struct server_rpc* pending = &svin_pending[idx];
			if(pending->type == SRPC_MINECART_CONTROL
			   && pending->payload.minecart_control.entity_id
				  == call->payload.minecart_control.entity_id
			   && pending->payload.minecart_control.ride_epoch
				  == call->payload.minecart_control.ride_epoch) {
				if(svin_serial_newer(
					   call->payload.minecart_control.input_seq,
					   pending->payload.minecart_control.input_seq))
					svin_pending[idx] = *call;
				return true;
			}
		}
	}

	/* Ride intent is idempotent for the same target within one input epoch. Only
	 * a serial-newer request replaces the pending one at its original FIFO
	 * position; equal or older retries are already represented and return true
	 * without replacing the queued newest desired state. A different target is
	 * a distinct ownership action and must retry after this reserved slot drains;
	 * replacing it here would strand the old target's optimistic client state. */
	if(call->type == SRPC_MINECART_RIDE_INTENT) {
		for(size_t i = 0; i < svin_pending_len; i++) {
			size_t idx = (svin_pending_head + i) % SVIN_PENDING_SIZE;
			if(!svin_source_equal(source, svin_pending_source[idx])) continue;
			if(svin_pending[idx].type == SRPC_MINECART_RIDE_INTENT
			   && svin_pending[idx].payload.minecart_ride_intent.entity_id
				  == call->payload.minecart_ride_intent.entity_id
			   && svin_pending[idx].payload.minecart_ride_intent.input_epoch
				  == call->payload.minecart_ride_intent.input_epoch) {
				if(svin_serial_newer(
					   call->payload.minecart_ride_intent.request_seq,
					   svin_pending[idx]
						  .payload.minecart_ride_intent.request_seq))
					svin_pending[idx] = *call;
				return true;
			}
		}
	}

	/* The live split switch carries a DESIRED state for one world generation,
	 * so a pending request is fully represented by the newest one: replace its
	 * payload at the original FIFO position rather than consuming the reserve
	 * twice. Never across generations -- a request stamped for a different
	 * generation is a different decision (and the server rejects all but its
	 * own current one), so it queues or is rejected on its own merits. */
	if(call->type == SRPC_SPLIT_SET) {
		for(size_t i = 0; i < svin_pending_len; i++) {
			size_t idx = (svin_pending_head + i) % SVIN_PENDING_SIZE;
			if(!svin_source_equal(source, svin_pending_source[idx])) continue;
			if(svin_pending[idx].type == SRPC_SPLIT_SET
			   && svin_pending[idx].payload.split_set.world_generation
				  == call->payload.split_set.world_generation) {
				if(svin_serial_newer(
					   call->payload.split_set.request_seq,
					   svin_pending[idx].payload.split_set.request_seq))
					svin_pending[idx] = *call;
				return true;
			}
		}
	}

	bool lifecycle = svin_is_lifecycle(call->type);
	bool input_ack = svin_is_input_ack(call->type);
	bool stream_recover = svin_is_stream_recover(call->type);
	bool player_state = svin_is_player_state(call->type);
	bool minecart_control = svin_is_minecart_control(call->type);
	bool ride_intent = svin_is_ride_intent(call->type);
	bool split_set = svin_is_split_set(call->type);
	bool motion = svin_is_motion(call->type); /* local-motion mode only */
	/* the control reserve is a LOCAL-session guarantee; remote/legacy keeps
	 * the pre-round-3 ordinary-class contract exactly */
	bool control = svin_local_motion_on && svin_is_control(call->type);
	size_t ordinary_count
		= svin_pending_len - svin_pending_lifecycle_count[0] - svin_pending_lifecycle_count[1]
		  - svin_pending_input_ack_count - svin_pending_stream_recover_count
		  - svin_pending_player_state_count[0] - svin_pending_player_state_count[1]
		  - svin_pending_minecart_control_count
		  - svin_pending_ride_intent_count - svin_pending_split_set_count
		  - svin_pending_motion_count[0] - svin_pending_motion_count[1]
		  - svin_pending_control_count[0] - svin_pending_control_count[1];
	/* The per-owner census must agree with the derived total, or the cap below
	 * would be computed against a drifted number. Cheap invariant, asserts-on
	 * in every test target (CMakeLists forces -UNDEBUG on test_*). */
	assert(ordinary_count
		   == svin_pending_action_count[0] + svin_pending_action_count[1]);
	/* S1-a2 / D4 per-owner ordinary cap. See SVIN_ACTION_OWNER_RESERVE. */
	size_t own_action_cap = SVIN_ACTION_PENDING_SIZE;
	if(source.slot == 1
	   || __atomic_load_n(&svin_split_accept, __ATOMIC_RELAXED))
		own_action_cap = SVIN_ACTION_PENDING_SIZE - SVIN_ACTION_OWNER_RESERVE;
	if(motion
	   && (svin_pending_motion_count[source.slot] >= SVIN_LOCAL_MOTION_RESERVE
		   || svin_pending_len >= SVIN_PENDING_SIZE)) {
		/* The producer contract (svin_motion_room before authoring) makes
		 * this unreachable from the game; counted twice so it can never be
		 * silent (F3 rpcdrop row and the F1 refusal count). */
		svin_motion_refused++;
		svin_dropped++;
		return false;
	}
	if((input_ack
		&& (svin_pending_input_ack_count >= SVIN_INPUT_ACK_RESERVE
			|| svin_pending_len >= SVIN_PENDING_SIZE))
	   || (stream_recover
		&& (svin_pending_stream_recover_count >= SVIN_STREAM_RECOVER_RESERVE
			|| svin_pending_len >= SVIN_PENDING_SIZE))
	   || (player_state
		&& (svin_pending_player_state_count[source.slot]
				>= (source.slot ? SVIN_GUEST_PLAYER_STATE_RESERVE
								: SVIN_PLAYER_STATE_RESERVE)
			|| svin_pending_len >= SVIN_PENDING_SIZE))
	   || (minecart_control
		&& (svin_pending_minecart_control_count
				>= SVIN_MINECART_CONTROL_RESERVE
			|| svin_pending_len >= SVIN_PENDING_SIZE))
	   || (ride_intent
		&& (svin_pending_ride_intent_count >= SVIN_RIDE_INTENT_RESERVE
			|| svin_pending_len >= SVIN_PENDING_SIZE))
	   || (split_set
		&& (svin_pending_split_set_count >= SVIN_SPLIT_SET_RESERVE
			|| svin_pending_len >= SVIN_PENDING_SIZE))
	   || (lifecycle
		&& (svin_pending_lifecycle_count[source.slot]
			>= (source.slot ? SVIN_GUEST_CLOSE_RESERVE : SVIN_LIFECYCLE_RESERVE)
			|| svin_pending_len >= SVIN_PENDING_SIZE))
	   || (control
		   && svin_pending_control_count[source.slot] >= SVIN_CONTROL_RESERVE
		   && (ordinary_count >= SVIN_ACTION_PENDING_SIZE
			   || svin_pending_action_count[source.slot] >= own_action_cap
			   || svin_pending_len >= SVIN_PENDING_SIZE))
	   || (!lifecycle && !input_ack && !stream_recover && !player_state
		   && !minecart_control && !ride_intent && !split_set && !motion
		   && !control
		   && (ordinary_count >= SVIN_ACTION_PENDING_SIZE
			   || svin_pending_action_count[source.slot] >= own_action_cap
			   || svin_pending_len >= SVIN_PENDING_SIZE))) {
		// Never evict an earlier action: a WINDOW_CLICK may own an optimistic
		// client revision that only its transaction reply can reconcile.
		svin_dropped++;
		return false;
	}

	svin_pending[(svin_pending_head + svin_pending_len) % SVIN_PENDING_SIZE] = *call;
	svin_pending_source[(svin_pending_head + svin_pending_len) % SVIN_PENDING_SIZE] = source;
	svin_pending_len++;
	if(lifecycle)
		svin_pending_lifecycle_count[source.slot]++;
	else if(input_ack)
		svin_pending_input_ack_count++;
	else if(stream_recover)
		svin_pending_stream_recover_count++;
	else if(player_state)
		svin_pending_player_state_count[source.slot]++;
	else if(minecart_control)
		svin_pending_minecart_control_count++;
	else if(ride_intent)
		svin_pending_ride_intent_count++;
	else if(split_set)
		svin_pending_split_set_count++;
	else if(motion)
		svin_pending_motion_count[source.slot]++;
	else if(control
			&& (ordinary_count >= SVIN_ACTION_PENDING_SIZE
				|| svin_pending_action_count[source.slot] >= own_action_cap)) {
		/* the ordinary class is full: the dedicated reserve */
		svin_pending_control_count[source.slot]++;
		svin_pending_in_control[(svin_pending_head + svin_pending_len - 1)
								% SVIN_PENDING_SIZE] = true;
	} else
		svin_pending_action_count[source.slot]++; /* reserve, on ACCEPT */
	return true;
}

/* ===== S1-a2 / Astra gate-2 D4: TEARDOWN release ==========================
 * A guest session ending (leave, death-release, world switch, a rejoin under a
 * new generation) must not leave its queued actions occupying the shared
 * bound: they can never be dispatched usefully -- the server re-validates the
 * generation and drops them -- but until they drained they would keep counting
 * against both the total and the departing owner's reservation, and a rejoin
 * would inherit a queue full of the previous session's requests. So drop them
 * HERE, releasing the reservation as part of the same operation. ONE exception
 * (R-S1a2 F5): a queued SRPC_WINDOW_CLOSE is kept -- see the in-loop comment.
 *
 * Compacts IN PLACE with no scratch buffer, preserving the RELATIVE FIFO order
 * of every entry that is kept (so per-owner ordering is preserved for the
 * surviving owner -- D4's third condition). The write position is
 * (head + kept) % SIZE and kept <= i always, so it only ever lands in the
 * already-read prefix of the ring: no unread entry can be clobbered, and the
 * head does not move. (A scratch copy of the whole queue would be a ~KB stack
 * frame; this queue is touched from the render thread on a 128KB Wii stack.)
 * Slot 0 is never droppable: the host has no session boundary to cross.
 * generation 0 means "every entry from that slot, whatever its generation".
 * Returns how many entries were dropped.
 *
 * THREADING + the production call site. Like every other function on this
 * queue this is CLIENT/MAIN-thread only (the queue is deliberately unlocked).
 * A guest session ends on the SERVER thread, so the server may NOT call this;
 * the main-thread call site is the client's own LOCAL_GUEST_LEFT /
 * generation-change handling, which is S1-b's (client_interface.c /
 * client_session.c are outside this brief's owned paths -- S1-a2 must not
 * touch client files). Until S1-b wires it, the reservation of a departed
 * session still releases -- just at DISPATCH latency rather than immediately:
 * the flush drains the queue every frame, the server drops each stale-
 * generation entry without mutation, and svin_rpc_flush_pending decrements the
 * owner's census as it hands each one over. svin_init releases everything at a
 * world/session re-init. So this is a bounded-latency improvement plus the
 * rejoin-inherits-a-full-queue fix, not the only release path. Exercised
 * directly by tests/test_svin_pending_equiv.c. */
size_t svin_rpc_drop_owner(unsigned slot, uint64_t generation) {
	if(slot != 1)
		return 0;
	size_t kept = 0, dropped = 0;
	for(size_t i = 0; i < svin_pending_len; i++) {
		size_t idx = (svin_pending_head + i) % SVIN_PENDING_SIZE;
		struct server_rpc_source src = svin_pending_source[idx];
		/* R-S1a2 F5: a queued SRPC_WINDOW_CLOSE SURVIVES the teardown drop.
		 * It is the container-ownership RELEASE, and the whole reason the
		 * control class carries a dedicated SVIN_GUEST_CLOSE_RESERVE slot --
		 * dropping it here would quietly break that reserve's contract (the
		 * one message a departing guest is always guaranteed to be able to
		 * queue would be the one thrown away). Keeping it costs a single
		 * control slot and never changes the outcome for a stale session: the
		 * server re-validates the generation at dispatch and drops a stale
		 * close without mutating anything. Every OTHER class of the departing
		 * owner's traffic is still dropped, releasing its action reservation
		 * immediately as before. */
		bool drop = src.slot == slot
			&& (generation == 0 || src.generation == generation)
			&& svin_pending[idx].type != SRPC_WINDOW_CLOSE;
		if(!drop) {
			size_t wp = (svin_pending_head + kept) % SVIN_PENDING_SIZE;
			if(wp != idx) {
				svin_pending[wp] = svin_pending[idx];
				svin_pending_source[wp] = src;
				svin_pending_in_control[wp] = svin_pending_in_control[idx];
				svin_pending_in_control[idx] = false;
			}
			kept++;
			continue;
		}
		dropped++;
		if(svin_is_lifecycle(svin_pending[idx].type)) {
			assert(svin_pending_lifecycle_count[src.slot] > 0);
			svin_pending_lifecycle_count[src.slot]--;
		} else if(svin_is_player_state(svin_pending[idx].type)) {
			assert(svin_pending_player_state_count[src.slot] > 0);
			svin_pending_player_state_count[src.slot]--;
		} else if(svin_is_input_ack(svin_pending[idx].type)) {
			assert(svin_pending_input_ack_count > 0);
			svin_pending_input_ack_count--;
		} else if(svin_is_stream_recover(svin_pending[idx].type)) {
			assert(svin_pending_stream_recover_count > 0);
			svin_pending_stream_recover_count--;
		} else if(svin_is_minecart_control(svin_pending[idx].type)) {
			assert(svin_pending_minecart_control_count > 0);
			svin_pending_minecart_control_count--;
		} else if(svin_is_ride_intent(svin_pending[idx].type)) {
			assert(svin_pending_ride_intent_count > 0);
			svin_pending_ride_intent_count--;
		} else if(svin_is_split_set(svin_pending[idx].type)) {
			assert(svin_pending_split_set_count > 0);
			svin_pending_split_set_count--;
		} else if(svin_is_motion(svin_pending[idx].type)) {
			assert(svin_pending_motion_count[src.slot] > 0);
			svin_pending_motion_count[src.slot]--;
			svin_credit_release(&svin_pending[idx]);
		} else if(svin_pending_in_control[idx]) {
			assert(svin_pending_control_count[src.slot] > 0);
			svin_pending_control_count[src.slot]--;
			svin_pending_in_control[idx] = false;
		} else {
			assert(svin_pending_action_count[src.slot] > 0);
			svin_pending_action_count[src.slot]--;
		}
	}
	svin_pending_len = kept;
	return dropped;
}
