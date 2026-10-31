/*
	test_svin_reliable -- pins the reliable-delivery semantics of the client->server
	RPC inbox (server_interface.c).

	THE BUG THIS GUARDS (real-Wii playtest 2026-06-07): under in-play lag the
	16-slot server inbox fills, and the old svin_rpc_send dropped WHATEVER message
	arrived next -- including SRPC_BLOCK_DIG / SRPC_BLOCK_PLACE. The player saw
	"blocks are not mined". Beta's client->server channel is TCP: actions are
	delayed under load, never lost.

	Pinned semantics:
	  1. With a full inbox, SRPC_PLAYER_POS is DROPPED (idempotent, re-sent every
	     50ms) and counted in svin_dropped_count().
	  2. With a full inbox, player actions are QUEUED (svin_pending_count grows),
	     never dropped.
	  3. Once the inbox drains, svin_rpc_flush_pending delivers the queued actions
	     IN ORDER, interleaved correctly with later direct sends (a dig
	     finished=true never overtakes its finished=false).
	  4. Pending overflow (SVIN_PENDING_SIZE exceeded) drops + counts, never
	     blocks, never corrupts the ring.
	  5. A full action backlog still admits the newest movement-state snapshot,
	     cumulative minecart control, close/unload/load, respawn ACK and
	     live-stream recovery in independent reserved slots without changing
	     their exact FIFO order.
	  6. Same-epoch minecart intents for different entity IDs never overwrite one
	     another; the first target reaches authority before the retarget retries.

	Headless, links only server_interface.c + platform/thread.c. Asserts-on
	always (-UNDEBUG forced by CMakeLists on test_ targets).
*/

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/network/server_interface.h"

#define RPC_INBOX_SIZE 16	 // must match server_interface.c
#define SVIN_ACTION_PENDING_SIZE 64 // must match server_interface.c
#define SVIN_LIFECYCLE_RESERVE 3
#define SVIN_INPUT_ACK_RESERVE 1
#define SVIN_STREAM_RECOVER_RESERVE 1
#define SVIN_PLAYER_STATE_RESERVE 1
#define SVIN_MINECART_CONTROL_RESERVE 1
#define SVIN_RIDE_INTENT_RESERVE 1
#define SVIN_PENDING_SIZE \
	(SVIN_ACTION_PENDING_SIZE + SVIN_LIFECYCLE_RESERVE \
	 + SVIN_INPUT_ACK_RESERVE + SVIN_STREAM_RECOVER_RESERVE \
	 + SVIN_PLAYER_STATE_RESERVE + SVIN_MINECART_CONTROL_RESERVE \
	 + SVIN_RIDE_INTENT_RESERVE)

static struct server_rpc received[256];
static size_t received_count = 0;

static void recorder(struct server_rpc* call, void* user) {
	(void)user;
	assert(received_count < sizeof(received) / sizeof(received[0]));
	received[received_count++] = *call;
}

static void drain(void) {
	svin_process_messages(recorder, NULL, false);
}

static struct server_rpc pos_msg(void) {
	return (struct server_rpc) {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = 1, .y = 2, .z = 3},
	};
}

static struct server_rpc dig_msg(bool finished, w_coord_t x) {
	return (struct server_rpc) {
		.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {.finished = finished, .x = x, .y = 64, .z = 0},
	};
}

static struct server_rpc minecart_control_msg(
	uint32_t entity_id, uint32_t ride_epoch, uint32_t input_seq,
	uint32_t positive_steps, uint32_t negative_steps) {
	return (struct server_rpc) {
		.type = SRPC_MINECART_CONTROL,
		.payload.minecart_control = {
			.entity_id = entity_id,
			.ride_epoch = ride_epoch,
			.input_seq = input_seq,
			.positive_steps = positive_steps,
			.negative_steps = negative_steps,
		},
	};
}

static struct server_rpc ride_intent_msg(
	uint32_t entity_id, uint32_t input_epoch, uint32_t request_seq,
	bool desired_occupied) {
	return (struct server_rpc) {
		.type = SRPC_MINECART_RIDE_INTENT,
		.payload.minecart_ride_intent = {
			.entity_id = entity_id,
			.input_epoch = input_epoch,
			.request_seq = request_seq,
			.desired_occupied = desired_occupied,
		},
	};
}

static struct server_rpc_source received_source[256];
static void owned_recorder(struct server_rpc_source source,
	struct server_rpc* call, void* user) {
	uint32_t active = svin_active_owned_rpc_marker();
	assert(active == (((uint32_t)source.slot << 16)
		| ((uint32_t)call->type + 1u)));
	received_source[received_count] = source;
	recorder(call, user);
}
static void test_guest_queue(void) {
	received_count = 0;
	struct server_rpc_source guest = {.slot=1,.generation=41};
	struct server_rpc_source later = {.slot=1,.generation=42};
	struct server_rpc click = {.type=SRPC_WINDOW_CLICK,
		.payload.window_click={.window=2,.slot=0,.action_id=42}};
	struct server_rpc close = {.type=SRPC_WINDOW_CLOSE,.payload.window_close.window=2};
	/* Fill both tiers, alternating senders with identical action/window IDs. */
	for(int k=0;k<RPC_INBOX_SIZE+SVIN_ACTION_PENDING_SIZE;k++)
		assert(svin_rpc_send_from(k%2 ? guest : (struct server_rpc_source){0}, &click));
	assert(!svin_rpc_send_from(guest, &click));
	assert(svin_rpc_send(&close));
	assert(svin_rpc_send_from(guest, &close));
	assert(svin_rpc_send_from(guest, &close)); /* exact owner retry coalesces */
	assert(svin_pending_count()==SVIN_ACTION_PENDING_SIZE+2);
	/* A later occupant cannot overwrite the former occupant's queued close. */
	assert(!svin_rpc_send_from(later, &close));
	struct server_rpc unload = {.type=SRPC_UNLOAD_WORLD};
	assert(!svin_rpc_send_from(guest, &unload));
	assert(!svin_rpc_send_from((struct server_rpc_source){.slot=2,.generation=1}, &click));
	assert(!svin_rpc_send_from((struct server_rpc_source){.slot=1}, &click));
	assert(!svin_rpc_send_from((struct server_rpc_source){.generation=1}, &click));
	do {
		svin_process_owned_messages(owned_recorder,NULL,false);
		svin_rpc_flush_pending();
	} while(svin_pending_count());
	svin_process_owned_messages(owned_recorder,NULL,false);
	assert(svin_active_owned_rpc_marker() == 0);
	assert(received_count==RPC_INBOX_SIZE+SVIN_ACTION_PENDING_SIZE+2);
	for(size_t k=0;k<received_count;k++) {
		assert(svin_source_equal(received_source[k], k%2 ? guest : (struct server_rpc_source){0}));
		if(k<RPC_INBOX_SIZE+SVIN_ACTION_PENDING_SIZE)
			assert(received[k].type==SRPC_WINDOW_CLICK && received[k].payload.window_click.action_id==42);
		else assert(received[k].type==SRPC_WINDOW_CLOSE);
	}
	/* Legacy/remote consumers must discard guest traffic, never impersonate P1. */
	received_count=0;
	assert(svin_rpc_send_from(guest,&click));
	assert(svin_rpc_send(&close));
	drain();
	assert(received_count==1 && received[0].type==SRPC_WINDOW_CLOSE);
	puts("PASS guest queue: sender survives saturation; private close reserves; legacy provider rejects guests");
}

/* ======================================================================
 * V6 F1 (ASTRA-V6-PLAN item 3; ASTRA-V5-REVIEW R1/R2): LOCAL motion
 * losslessness and pause/resume/leave progress at the transport.
 * Every sample carries its authoring index in x and its owner in z, so the
 * delivered stream can be checked for exact count, order and payload.
 * ====================================================================== */
#define F1_LOG 2048
static struct server_rpc f1_got[F1_LOG];
static struct server_rpc_source f1_src[F1_LOG];
static uint32_t f1_ctx[F1_LOG];
static size_t f1_n;
static int f1_budget;              /* messages admitted per drain (Beta 101) */
static bool f1_storage_full[2];    /* fake server: motion storage full */
static bool f1_paused;             /* fake server: pause state */
static bool f1_ends[2];            /* fake server: unload/leave ends the lifetime */
static uint64_t f1_life[2];        /* fake server: lifetime identity */
static size_t f1_cancel_n;
static struct server_rpc f1_cancelled[F1_LOG];

static void f1_process(struct server_rpc_source src, struct server_rpc* call,
	void* user) {
	(void)user;
	assert(f1_n < F1_LOG);
	f1_src[f1_n] = src;
	f1_ctx[f1_n] = svin_dispatch_context();
	f1_got[f1_n++] = *call;
	if(call->type == SRPC_TOGGLE_PAUSE)
		f1_paused = !f1_paused;
	if((call->type == SRPC_UNLOAD_WORLD || call->type == SRPC_LOCAL_GUEST)
	   && f1_ends[src.slot ? 1 : 0])
		f1_life[src.slot ? 1 : 0]++;
}
static enum svin_admit_result f1_admit(struct server_rpc_source src,
	const struct server_rpc* call, void* user) {
	(void)user;
	if(f1_budget <= 0)
		return SVIN_DEFER;
	if(call->type == SRPC_PLAYER_POS && f1_storage_full[src.slot ? 1 : 0])
		return SVIN_DEFER_STORAGE;
	f1_budget--;
	return SVIN_ADMIT;
}
static bool f1_control(struct server_rpc_source src, const struct server_rpc* call,
	void* user) {
	(void)user;
	if(src.slot == 0)
		return call->type == SRPC_TOGGLE_PAUSE || call->type == SRPC_UNLOAD_WORLD;
	return call->type == SRPC_LOCAL_GUEST
		&& call->payload.local_guest.action == LOCAL_GUEST_LEAVE;
}
static uint32_t f1_context(struct server_rpc_source src, void* user) {
	(void)src;
	(void)user;
	return UINT32_C(0x80000000) | (f1_paused ? 1u : 0u);
}
static uint64_t f1_lifetime(struct server_rpc_source src, void* user) {
	(void)user;
	return f1_life[src.slot ? 1 : 0];
}
static void f1_cancel(struct server_rpc_source src, struct server_rpc* call,
	void* user) {
	(void)src;
	(void)user;
	assert(f1_cancel_n < F1_LOG);
	f1_cancelled[f1_cancel_n++] = *call;
}
static const struct svin_drain_ops f1_ops = {
	.process = f1_process, .admit = f1_admit, .control = f1_control,
	.context = f1_context, .lifetime = f1_lifetime, .cancel = f1_cancel,
};
static void f1_drain(int budget) {
	f1_budget = budget;
	svin_process_owned_messages_ops(&f1_ops, NULL);
}
static struct server_rpc f1_pos(int index, unsigned owner) {
	return (struct server_rpc) {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = index, .y = 64, .z = owner,
							   .correction_seq = 0},
	};
}
static struct server_rpc f1_hotbar(int index) {
	return (struct server_rpc) {
		.type = SRPC_HOTBAR_SLOT, .payload.hotbar_slot.slot = (size_t)index,
	};
}
/* V6 F1 round 3: every LOCAL movement sample is a credited publication
 * (reserve before the tick, publish after it). false = no credit. */
static bool f1_csend(struct server_rpc_source src, int index, unsigned owner) {
	if(!svin_credit_reserve(src))
		return false;
	struct server_rpc m = f1_pos(index, owner);
	svin_credit_publish(src, &m);
	return true;
}
static void f1_reset(void) {
	assert(svin_init());
	svin_set_local_motion(true);
	f1_n = 0;
	f1_cancel_n = 0;
	f1_storage_full[0] = f1_storage_full[1] = false;
	f1_paused = false;
	f1_ends[0] = f1_ends[1] = false;
	f1_life[0] = f1_life[1] = 1;
}

/* L1: the 16 inbox tokens are exhausted -- every one HELD by the server
 * (in flight, not drained) -- then the producer keeps authoring. Each sample
 * it authors must be accepted (never dropped), stored in order, and
 * delivered exactly once; when its owner's bound is reached the producer is
 * told BEFORE authoring (svin_motion_room) and refills as the server drains.
 * Ordinary actions are interleaved every 7th sample. N = 32/33/34/40/101 and
 * the sustained 250-sample refill. */
static int f1_expect[F1_LOG][2]; /* {type, index} in authoring order */
static void f1_lossless_host(int total) {
	f1_reset();
	const struct server_rpc_source host = {0};
	int authored = 0, actions = 0, backpressure = 0;
	int expect_n = 0;
	/* the 16 tokens: in the inbox, then held by a server whose budget is 0 */
	for(; authored < RPC_INBOX_SIZE; authored++) {
		assert(f1_csend(host, authored, 0));
		f1_expect[expect_n][0] = SRPC_PLAYER_POS;
		f1_expect[expect_n++][1] = authored;
	}
	f1_drain(0);
	assert(svin_held_count() == RPC_INBOX_SIZE && f1_n == 0);
	const uint32_t drop0 = svin_dropped_count();
	int drains = 0;
	while(authored < total) {
		if(!f1_csend(host, authored, 0)) {
			/* backpressure: nothing authored; the server drains one update */
			backpressure++;
			f1_drain(101);
			svin_rpc_flush_pending();
			assert(++drains < 64); /* finite progress */
			continue;
		}
		f1_expect[expect_n][0] = SRPC_PLAYER_POS;
		f1_expect[expect_n++][1] = authored;
		authored++;
		if(authored % 7 == 0) {
			struct server_rpc a = f1_hotbar(actions);
			assert(svin_rpc_send(&a));
			f1_expect[expect_n][0] = SRPC_HOTBAR_SLOT;
			f1_expect[expect_n++][1] = actions++;
		}
	}
	for(int k = 0; svin_held_count() + svin_pending_count() > 0
					|| f1_n < (size_t)expect_n; k++) {
		assert(k < 64); /* every message reaches the server in finite time */
		f1_drain(101);
		svin_rpc_flush_pending();
	}
	assert(f1_n == (size_t)expect_n);
	for(int k = 0; k < expect_n; k++) {
		assert((int)f1_got[k].type == f1_expect[k][0]);
		assert(svin_source_equal(f1_src[k], host) && f1_ctx[k] == 0);
		if(f1_expect[k][0] == SRPC_PLAYER_POS)
			assert(f1_got[k].payload.player_pos.x == (double)f1_expect[k][1]
				   && f1_got[k].payload.player_pos.z == 0.0);
		else
			assert(f1_got[k].payload.hotbar_slot.slot == (size_t)f1_expect[k][1]);
	}
	assert(svin_dropped_count() == drop0 && svin_motion_refused_count() == 0);
	printf("  F1 L1 host %d samples (+%d actions) behind 16 held tokens: all delivered once in order, %d backpressure waits, 0 dropped OK\n",
		   total, actions, backpressure);
	svin_set_local_motion(false);
}

/* L2: both owners. The host's bound being reached never consumes the guest's
 * room (and vice versa); per-source order and payload exact. */
static void f1_lossless_both(void) {
	f1_reset();
	svin_set_split_accept(true);
	const struct server_rpc_source host = {0};
	const struct server_rpc_source guest = {.slot = 1, .generation = 9};
	int h = 0, g = 0;
	/* exhaust the tokens with held guest samples */
	for(; g < RPC_INBOX_SIZE; g++) {
		assert(f1_csend(guest, g, 1));
	}
	f1_drain(0);
	assert(svin_held_count() == RPC_INBOX_SIZE);
	/* the host fills to its bound */
	while(f1_csend(host, h, 0)) {
		h++;
		assert(h < 1000);
	}
	assert(h > 0 && svin_motion_pending_count(0) == (size_t)h);
	for(int k = 0; k < 40; k++) { /* the guest's credits are its own */
		assert(f1_csend(guest, g, 1));
		g++;
	}
	for(int k = 0; svin_held_count() + svin_pending_count() > 0; k++) {
		assert(k < 64);
		f1_drain(101);
		svin_rpc_flush_pending();
	}
	f1_drain(101); /* the inbox the last flush filled */
	int next[2] = {0, 0};
	for(size_t k = 0; k < f1_n; k++) {
		assert(f1_got[k].type == SRPC_PLAYER_POS);
		const unsigned o = f1_src[k].slot;
		assert(f1_got[k].payload.player_pos.z == (double)o);
		assert(f1_got[k].payload.player_pos.x == (double)next[o]);
		next[o]++;
	}
	assert(next[0] == h && next[1] == g && svin_motion_refused_count() == 0);
	printf("  F1 L2 both owners: host %d (bound reached) + guest %d behind 16 held guest tokens, per-owner room independent, exact order OK\n",
		   h, g);
	(void)host;
	svin_set_split_accept(false);
	svin_set_local_motion(false);
}

/* L3: a FULL paused backlog with the unpause behind it. The server's motion
 * storage is full (DEFER_STORAGE) and only an unpaused update can free it, so
 * FIFO alone deadlocks. The unpause must progress in finite time, every
 * overtaken message must be processed later, in order, under the state it
 * would have seen in FIFO (stamped "paused"), and later traffic unstamped. */
static void f1_pause_progress(void) {
	f1_reset();
	const struct server_rpc_source host = {0};
	f1_paused = true;
	f1_storage_full[0] = true;
	struct server_rpc m;
	assert(f1_csend((struct server_rpc_source){0}, 0, 0));
	m = f1_hotbar(0); assert(svin_rpc_send(&m));
	assert(f1_csend((struct server_rpc_source){0}, 1, 0));
	m = (struct server_rpc) {.type = SRPC_TOGGLE_PAUSE};
	assert(svin_rpc_send(&m));
	assert(f1_csend((struct server_rpc_source){0}, 2, 0));
	int drains = 0;
	while(f1_paused) {
		assert(++drains <= 2); /* the resume progresses in finite time */
		f1_drain(101);
	}
	assert(f1_n == 1 && f1_got[0].type == SRPC_TOGGLE_PAUSE && f1_ctx[0] == 0);
	f1_storage_full[0] = false; /* the unpaused update replayed the queue */
	f1_drain(101);
	assert(f1_n == 5);
	assert(f1_got[1].type == SRPC_PLAYER_POS && f1_got[1].payload.player_pos.x == 0
		   && f1_ctx[1] == (UINT32_C(0x80000000) | 1u));
	assert(f1_got[2].type == SRPC_HOTBAR_SLOT
		   && f1_ctx[2] == (UINT32_C(0x80000000) | 1u));
	assert(f1_got[3].type == SRPC_PLAYER_POS && f1_got[3].payload.player_pos.x == 1
		   && f1_ctx[3] == (UINT32_C(0x80000000) | 1u));
	assert(f1_got[4].type == SRPC_PLAYER_POS && f1_got[4].payload.player_pos.x == 2
		   && f1_ctx[4] == 0);
	for(size_t k = 0; k < f1_n; k++)
		assert(svin_source_equal(f1_src[k], host));
	assert(svin_held_count() == 0 && svin_cancelled_count() == 0);
	puts("  F1 L3 full paused backlog + resume behind it: resume in 1 drain, overtaken 3 replayed in order stamped paused, later sample unstamped OK");
	svin_set_local_motion(false);
}

/* L4: unload / leave behind a full paused backlog. A lifetime-ending control
 * progresses; the input of the ended lifetime it overtook is CANCELLED
 * (never processed), traffic after it survives, the other owner is
 * untouched. A refused leave (lifetime survives) cancels nothing: the
 * overtaken input is processed later, stamped, exactly once. */
static void f1_leave_progress(void) {
	f1_reset();
	svin_set_split_accept(true);
	const struct server_rpc_source guest = {.slot = 1, .generation = 5};
	f1_paused = true;
	f1_storage_full[0] = f1_storage_full[1] = true;
	struct server_rpc m;
	assert(f1_csend((struct server_rpc_source){0}, 0, 0));
	assert(f1_csend(guest, 0, 1));
	m = f1_hotbar(1); assert(svin_rpc_send(&m));
	assert(f1_csend(guest, 1, 1));
	m = (struct server_rpc) {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = 3, .action = LOCAL_GUEST_LEAVE,
								.owner = 1, .generation = 5}};
	assert(svin_rpc_send_from(guest, &m));
	m = (struct server_rpc) {.type = SRPC_UNLOAD_WORLD};
	assert(svin_rpc_send(&m));
	m = (struct server_rpc) {.type = SRPC_LOAD_WORLD};
	assert(svin_rpc_send(&m));
	f1_ends[0] = f1_ends[1] = true; /* both controls end their lifetime */
	int drains = 0;
	while(f1_n < 2) {
		assert(++drains <= 2);
		f1_drain(101);
	}
	/* cross-source order is unspecified (per-connection FIFO only) */
	const size_t li = f1_got[0].type == SRPC_LOCAL_GUEST ? 0 : 1;
	assert(f1_got[li].type == SRPC_LOCAL_GUEST && f1_src[li].slot == 1);
	assert(f1_got[1 - li].type == SRPC_UNLOAD_WORLD && f1_src[1 - li].slot == 0);
	assert(f1_cancel_n == 4 && svin_cancelled_count() == 4);
	/* the LOAD after the unload is the new lifetime's: processed next */
	f1_storage_full[0] = f1_storage_full[1] = false;
	f1_drain(101);
	assert(f1_n == 3 && f1_got[2].type == SRPC_LOAD_WORLD && f1_ctx[2] == 0);
	assert(svin_held_count() == 0);
	/* a refused leave: nothing cancelled, overtaken input processed later */
	f1_reset();
	svin_set_split_accept(true);
	f1_paused = true;
	f1_storage_full[1] = true;
	assert(f1_csend(guest, 7, 1));
	m = (struct server_rpc) {.type = SRPC_LOCAL_GUEST,
		.payload.local_guest = {.ticket = 4, .action = LOCAL_GUEST_LEAVE,
								.owner = 1, .generation = 5}};
	assert(svin_rpc_send_from(guest, &m));
	assert(f1_csend((struct server_rpc_source){0}, 3, 0)); /* host unaffected */
	f1_drain(101);
	{ /* the host's own sample flows in the same drain (any cross order) */
		const size_t hi = f1_src[0].slot == 0 ? 0 : 1;
		assert(f1_n == 2 && f1_got[1 - hi].type == SRPC_LOCAL_GUEST
			   && f1_got[hi].type == SRPC_PLAYER_POS && f1_src[hi].slot == 0
			   && f1_got[hi].payload.player_pos.x == 3
			   && f1_ctx[hi] == 0 && f1_cancel_n == 0);
	}
	f1_storage_full[1] = false;
	f1_drain(101);
	assert(f1_n == 3 && f1_got[2].payload.player_pos.x == 7
		   && f1_src[2].slot == 1 && f1_ctx[2] == (UINT32_C(0x80000000) | 1u));
	puts("  F1 L4 leave + unload behind full paused backlogs: both progress, overtaken input of the ended lifetimes cancelled (4), later LOAD kept, refused leave cancels nothing OK");
	svin_set_split_accept(false);
	svin_set_local_motion(false);
}

/* L5: remote mode is unchanged -- a remote provider's PLAYER_POS still drops
 * at a full inbox, svin_motion_room always says yes. */
static void f1_remote_unchanged(void) {
	assert(svin_init());
	assert(!svin_local_motion());
	for(int k = 0; k < RPC_INBOX_SIZE; k++) {
		struct server_rpc m = pos_msg();
		assert(svin_rpc_send(&m));
	}
	struct server_rpc m = pos_msg();
	assert(svin_motion_room((struct server_rpc_source){0}));
	assert(!svin_rpc_send(&m) && svin_dropped_count() == 1
		   && svin_pending_count() == 0 && svin_motion_pending_count(0) == 0);
	puts("  F1 L5 remote mode: full-inbox PLAYER_POS still dropped + counted, no motion queue OK");
}

/* L6 (round 3, ASTRA-F1-DESIGN-RULING 4-8): credit conservation.
 * - every reservation also guarantees physical publication storage: with the
 *   consumer stopped, the host gets exactly the pending motion reserve plus
 *   the free tokens, never a publication that could be refused;
 * - outstanding counts reserved+pending+inbox+held until terminal
 *   settlement, which the drain performs when a handler does not take it;
 * - an abandoned reservation returns its credit;
 * - a release with a stale identity (an earlier local session) is ignored;
 * - controls (pause/resume, guest leave) stay enqueueable when the ordinary
 *   action class is full. */
static void f1_credit_accounting(void) {
	f1_reset();
	const struct server_rpc_source host = {0};
	int n = 0;
	while(f1_csend(host, n, 0))
		n++;
	printf("  F1 L6 consumer stopped: %d host publications reserved+published (the physical transport limit), outstanding %u\n",
		   n, svin_credit_outstanding(0));
	fflush(stdout);
	assert(n > RPC_INBOX_SIZE && n < SVIN_MOTION_CREDITS); /* disclosed: < 101 */
	assert(svin_credit_outstanding(0) == (uint32_t)n);
	assert(svin_motion_refused_count() == 0 && svin_dropped_count() == 0);
	/* an uncredited local PLAYER_POS is no escape hatch */
	{
		struct server_rpc raw = f1_pos(999, 0);
		assert(!svin_rpc_send(&raw));
	}
	/* the server holds all 16 (budget), nothing settles yet */
	f1_drain(0);
	assert(svin_credit_outstanding(0) == (uint32_t)n);
	/* drain everything: the fake handler takes nothing -> each settles */
	for(int k = 0; svin_held_count() + svin_pending_count() > 0; k++) {
		assert(k < 64);
		f1_drain(101);
		svin_rpc_flush_pending();
	}
	f1_drain(101);
	assert(f1_n == (size_t)n && svin_credit_outstanding(0) == 0);
	/* abandon */
	assert(svin_credit_reserve(host));
	assert(svin_credit_outstanding(0) == 1);
	svin_credit_abandon(host);
	assert(svin_credit_outstanding(0) == 0);
	/* stale identity: a sample of session A released after session B began */
	assert(f1_csend(host, 5000, 0));
	assert(svin_credit_outstanding(0) == 1);
	svin_set_local_motion(false);
	svin_set_local_motion(true); /* a new local session epoch */
	assert(svin_credit_outstanding(0) == 0);
	assert(f1_csend(host, 5001, 0));
	const uint32_t stale0 = svin_credit_stale_releases();
	f1_drain(101); /* settles both: A's is stale, B's is current */
	f1_drain(101);
	assert(svin_credit_outstanding(0) == 0
		   && svin_credit_stale_releases() == stale0 + 1);
	/* controls under a full ordinary class */
	f1_reset();
	for(int k = 0; k < RPC_INBOX_SIZE + SVIN_ACTION_PENDING_SIZE; k++) {
		struct server_rpc a = f1_hotbar(k % 9);
		assert(svin_rpc_send(&a));
	}
	{
		struct server_rpc a = f1_hotbar(1);
		assert(!svin_rpc_send(&a)); /* ordinary class full */
		struct server_rpc t = {.type = SRPC_TOGGLE_PAUSE};
		assert(svin_rpc_send(&t)); /* pause */
		assert(svin_rpc_send(&t)); /* resume */
	}
	puts("  F1 L6 credits: storage-guaranteed reservations, settle-on-dispatch, abandon, stale-session release ignored, uncredited local POS refused, pause/resume enqueueable past a full action class OK");
	svin_set_local_motion(false);
}

int main(void) {
	assert(svin_init());

	// --- 1. Fill the inbox completely with POS messages (server not draining).
	for(int k = 0; k < RPC_INBOX_SIZE; k++) {
		struct server_rpc m = pos_msg();
		svin_rpc_send(&m);
	}
	assert(svin_dropped_count() == 0);
	assert(svin_pending_count() == 0);

	// --- 2. Full inbox: POS drops, actions queue.
	{
		struct server_rpc m = pos_msg();
		svin_rpc_send(&m); // 17th POS -> dropped
	}
	assert(svin_dropped_count() == 1);
	assert(svin_pending_count() == 0);

	{
		struct server_rpc m = dig_msg(false, 100); // dig START
		svin_rpc_send(&m);
		m = dig_msg(true, 100); // dig FINISH
		svin_rpc_send(&m);
	}
	assert(svin_pending_count() == 2); // both queued, neither dropped
	assert(svin_dropped_count() == 1); // unchanged

	// Another POS while actions are pending: still dropped (never reorders
	// ahead of, or behind, the pending actions).
	{
		struct server_rpc m = pos_msg();
		svin_rpc_send(&m);
	}
	assert(svin_dropped_count() == 2);
	assert(svin_pending_count() == 2);

	// --- 3. Server drains; flush delivers pending IN ORDER, then later sends
	// follow them.
	drain();
	assert(received_count == RPC_INBOX_SIZE); // the 16 buffered POS
	received_count = 0;

	// A new send first flushes the backlog, then sends itself.
	{
		struct server_rpc m = dig_msg(false, 200);
		svin_rpc_send(&m);
	}
	assert(svin_pending_count() == 0); // backlog + new message all fit now

	drain();
	assert(received_count == 3);
	assert(received[0].type == SRPC_BLOCK_DIG
		   && received[0].payload.block_dig.finished == false
		   && received[0].payload.block_dig.x == 100);
	assert(received[1].type == SRPC_BLOCK_DIG
		   && received[1].payload.block_dig.finished == true
		   && received[1].payload.block_dig.x == 100);
	assert(received[2].type == SRPC_BLOCK_DIG
		   && received[2].payload.block_dig.x == 200);
	received_count = 0;

	// Explicit flush with nothing pending is a no-op.
	svin_rpc_flush_pending();
	drain();
	assert(received_count == 0);

	// --- 4. Pending overflow: fill inbox, queue SVIN_PENDING_SIZE actions,
	// then one more -- it must drop + count, not block or corrupt the ring.
	for(int k = 0; k < RPC_INBOX_SIZE; k++) {
		struct server_rpc m = pos_msg();
		svin_rpc_send(&m);
	}
	for(int k = 0; k < SVIN_ACTION_PENDING_SIZE; k++) {
		struct server_rpc m = dig_msg(false, 1000 + k);
		svin_rpc_send(&m);
	}
	assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE);
	uint32_t drops_before = svin_dropped_count();
	{
		struct server_rpc m = dig_msg(false, 9999);
		svin_rpc_send(&m); // 65th pending action -> counted drop
	}
	assert(svin_dropped_count() == drops_before + 1);
	assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE);

	// --- 5. Movement state and minecart-control reservations are independent
	// of the full action budget. Repeated snapshots coalesce to the newest state
	// at each first snapshot's FIFO position.
	struct server_rpc state = {
		.type = SRPC_PLAYER_STATE,
		.payload.player_state = {
			.on_ground = true, .sneaking = false, .correction_seq = 8,
		},
	};
	assert(svin_rpc_send(&state));
	assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE + 1);
	state.payload.player_state.on_ground = false;
	state.payload.player_state.sneaking = true;
	state.payload.player_state.correction_seq = 9;
	assert(svin_rpc_send(&state));
	assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE + 1);
	struct server_rpc control
		= minecart_control_msg(500, 7, 1, 1, 0);
	assert(svin_rpc_send(&control));
	assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE + 2);
	/* Newer cumulative state replaces the SAME cart+epoch entry at its first
	 * FIFO position. Equal and modularly older retries cannot regress it. */
	control = minecart_control_msg(500, 7, 4, 3, 1);
	assert(svin_rpc_send(&control));
	assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE + 2);
	struct server_rpc older_control
		= minecart_control_msg(500, 7, 2, 2, 0);
	assert(svin_rpc_send(&older_control));
	assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE + 2);
	/* A different epoch cannot steal the reserved slot or move new-session
	 * input ahead of the ownership actions that establish it. */
	uint32_t conflict_drops = svin_dropped_count();
	struct server_rpc next_epoch
		= minecart_control_msg(500, 8, 1, 1, 0);
	assert(!svin_rpc_send(&next_epoch));
	assert(svin_dropped_count() == conflict_drops + 1);
	assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE + 2);
	for(uint8_t window = 1; window <= SVIN_LIFECYCLE_RESERVE; window++) {
		struct server_rpc close = {
			.type = SRPC_WINDOW_CLOSE,
			.payload.window_close.window = window,
		};
		assert(svin_rpc_send(&close));
	}
	struct server_rpc ack = {
		.type = SRPC_INPUT_READY,
		.payload.input_ready.epoch = 77,
		.payload.input_ready.retry = false,
	};
	assert(svin_rpc_send(&ack));
	assert(svin_pending_count() == SVIN_PENDING_SIZE - 1 - SVIN_RIDE_INTENT_RESERVE);
	assert(svin_input_ready_pending(77));
	assert(svin_dropped_count() == drops_before + 2);
	// Same-epoch retry coalesces at the first ACK's exact FIFO position.
	assert(svin_rpc_send(&ack));
	assert(svin_pending_count() == SVIN_PENDING_SIZE - 1 - SVIN_RIDE_INTENT_RESERVE);
	/* A conflicting same-epoch NACK must fail closed without needing a second
	 * reserved slot: the original FIFO position becomes a retry request. */
	struct server_rpc nack = ack;
	nack.payload.input_ready.retry = true;
	assert(svin_rpc_send(&nack));
	assert(svin_pending_count() == SVIN_PENDING_SIZE - 1 - SVIN_RIDE_INTENT_RESERVE);
	/* Recovery has an independent ownership slot even when actions, lifecycle
	 * controls and the ACK reserve are all occupied. Same-generation retries
	 * coalesce at the first request's FIFO position. */
	struct server_rpc recover = {
		.type = SRPC_STREAM_RECOVER,
		.payload.stream_recover.epoch = 77,
	};
	assert(svin_rpc_send(&recover));
	assert(svin_pending_count() == SVIN_PENDING_SIZE - SVIN_RIDE_INTENT_RESERVE);
	assert(svin_rpc_send(&recover));
	assert(svin_pending_count() == SVIN_PENDING_SIZE - SVIN_RIDE_INTENT_RESERVE);
	/* Ride-intent fills the last reserved slot, completing the full ring.
	 * Same-epoch newer-seq coalesces in-place while all 72 entries are
	 * occupied -- no drop, count unchanged. */
	struct server_rpc ride = ride_intent_msg(100, 5, 1, true);
	assert(svin_rpc_send(&ride));
	assert(svin_pending_count() == SVIN_PENDING_SIZE);
	struct server_rpc ride_newer = ride_intent_msg(100, 5, 2, false);
	assert(svin_rpc_send(&ride_newer));
	assert(svin_pending_count() == SVIN_PENDING_SIZE
		   && svin_dropped_count() == drops_before + 2);
	/* The ordinary budget is still saturated while ACK is pending. Production
	 * must keep the input-less client gate up until the query above becomes false. */
	{
		struct server_rpc fresh = dig_msg(false, 10000);
		assert(!svin_rpc_send(&fresh));
	}
	assert(svin_dropped_count() == drops_before + 3);
	/* Coalescing must still succeed when every one of the 72 pending entries is
	 * occupied. A half-range candidate is ambiguous and must not replace it. */
	uint32_t full_drops = svin_dropped_count();
	control = minecart_control_msg(500, 7, 5, 4, 1);
	assert(svin_rpc_send(&control));
	assert(svin_pending_count() == SVIN_PENDING_SIZE
		   && svin_dropped_count() == full_drops);
	struct server_rpc half_range = minecart_control_msg(
		500, 7, UINT32_C(0x80000005), UINT32_C(0x80000004), 1);
	assert(svin_rpc_send(&half_range));
	assert(svin_pending_count() == SVIN_PENDING_SIZE
		   && svin_dropped_count() == full_drops);

	// Drain everything: 16 POS, then 64 digs, latest player state, latest
	// minecart control, 3 lifecycle controls, ACK, recovery, ride-intent.
	drain();
	assert(received_count == RPC_INBOX_SIZE);
	received_count = 0;
	while(svin_pending_count() > 0) {
		svin_rpc_flush_pending();
		drain();
	}
	assert(!svin_input_ready_pending(77));
	assert(received_count == SVIN_PENDING_SIZE);
	for(int k = 0; k < SVIN_ACTION_PENDING_SIZE; k++) {
		assert(received[k].type == SRPC_BLOCK_DIG);
		assert(received[k].payload.block_dig.x == 1000 + k); // order preserved
	}
	assert(received[SVIN_ACTION_PENDING_SIZE].type == SRPC_PLAYER_STATE);
	assert(!received[SVIN_ACTION_PENDING_SIZE]
			.payload.player_state.on_ground);
	assert(received[SVIN_ACTION_PENDING_SIZE]
			.payload.player_state.sneaking);
	assert(received[SVIN_ACTION_PENDING_SIZE]
			.payload.player_state.correction_seq == 9);
	assert(received[SVIN_ACTION_PENDING_SIZE + 1].type
		   == SRPC_MINECART_CONTROL);
	assert(received[SVIN_ACTION_PENDING_SIZE + 1]
			.payload.minecart_control.entity_id == 500);
	assert(received[SVIN_ACTION_PENDING_SIZE + 1]
			.payload.minecart_control.ride_epoch == 7);
	assert(received[SVIN_ACTION_PENDING_SIZE + 1]
			.payload.minecart_control.input_seq == 5);
	assert(received[SVIN_ACTION_PENDING_SIZE + 1]
			.payload.minecart_control.positive_steps == 4);
	assert(received[SVIN_ACTION_PENDING_SIZE + 1]
			.payload.minecart_control.negative_steps == 1);
	for(int k = 0; k < SVIN_LIFECYCLE_RESERVE; k++) {
		assert(received[SVIN_ACTION_PENDING_SIZE + 2 + k].type
			   == SRPC_WINDOW_CLOSE);
		assert(received[SVIN_ACTION_PENDING_SIZE + 2 + k]
				   .payload.window_close.window == k + 1);
	}
	assert(received[SVIN_PENDING_SIZE - 3].type == SRPC_INPUT_READY);
	assert(received[SVIN_PENDING_SIZE - 3].payload.input_ready.epoch == 77);
	assert(received[SVIN_PENDING_SIZE - 3].payload.input_ready.retry);
	assert(received[SVIN_PENDING_SIZE - 2].type == SRPC_STREAM_RECOVER);
	assert(received[SVIN_PENDING_SIZE - 2].payload.stream_recover.epoch == 77);
	assert(received[SVIN_PENDING_SIZE - 1].type == SRPC_MINECART_RIDE_INTENT);
	assert(received[SVIN_PENDING_SIZE - 1].payload.minecart_ride_intent.input_epoch == 5);
	assert(received[SVIN_PENDING_SIZE - 1].payload.minecart_ride_intent.request_seq == 2);
	assert(!received[SVIN_PENDING_SIZE - 1].payload.minecart_ride_intent.desired_occupied);

	/* Draining released the dedicated control reserve. A new epoch can acquire
	 * it normally; a stale per-reserve count would reject this forever. */
	for(int k = 0; k < RPC_INBOX_SIZE; k++) {
		struct server_rpc m = pos_msg();
		assert(svin_rpc_send(&m));
	}
	next_epoch = minecart_control_msg(500, 8, 1, 1, 0);
	assert(svin_rpc_send(&next_epoch));
	assert(svin_pending_count() == 1);
	drain();
	while(svin_pending_count() > 0) {
		svin_rpc_flush_pending();
		drain();
	}
	assert(received[received_count - 1].type == SRPC_MINECART_CONTROL);
	assert(received[received_count - 1]
			.payload.minecart_control.ride_epoch == 8);

	/* Put the control reserve FIRST, then fill all 64 ordinary action slots.
	 * A wrapped-newer sample must replace it without moving it past any action;
	 * an exact half-range sample must leave the wrapped value intact. */
	received_count = 0;
	for(int k = 0; k < RPC_INBOX_SIZE; k++) {
		struct server_rpc m = pos_msg();
		assert(svin_rpc_send(&m));
	}
	control = minecart_control_msg(600, 9, UINT32_MAX, UINT32_MAX, 0);
	assert(svin_rpc_send(&control));
	for(int k = 0; k < SVIN_ACTION_PENDING_SIZE; k++) {
		struct server_rpc m = dig_msg(false, 2000 + k);
		assert(svin_rpc_send(&m));
	}
	assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE + 1);
	control = minecart_control_msg(600, 9, 1, 1, 0);
	assert(svin_rpc_send(&control)); /* modular delta 2 */
	half_range = minecart_control_msg(
		600, 9, UINT32_C(0x80000001), UINT32_C(0x80000001), 0);
	assert(svin_rpc_send(&half_range));
	drain();
	received_count = 0;
	while(svin_pending_count() > 0) {
		svin_rpc_flush_pending();
		drain();
	}
	assert(received_count == SVIN_ACTION_PENDING_SIZE + 1);
	assert(received[0].type == SRPC_MINECART_CONTROL
		   && received[0].payload.minecart_control.input_seq == 1);
	for(int k = 0; k < SVIN_ACTION_PENDING_SIZE; k++) {
		assert(received[k + 1].type == SRPC_BLOCK_DIG);
		assert(received[k + 1].payload.block_dig.x == 2000 + k);
	}

	/* =====================================================================
	 * 7. RIDE-INTENT COALESCING + RESERVE PRESSURE: fill 16 inbox + 64
	 * ordinary pending slots, then exercise the ride-intent reserve.
	 * Pins: accepted in own slot; same-epoch newer replaces; same-epoch
	 * equal/older returns true without replacing; cross-epoch does not
	 * replace; after flush, all delivered.
	 * ===================================================================== */
	{
		received_count = 0;
		assert(svin_pending_count() == 0
			   && "prior scenario must leave the shared channel drained");

		/* Fill inbox completely. */
		for(int k = 0; k < RPC_INBOX_SIZE; k++) {
			struct server_rpc m = pos_msg();
			assert(svin_rpc_send(&m));
		}

		/* Fill all ordinary action pending slots. */
		for(int k = 0; k < SVIN_ACTION_PENDING_SIZE; k++) {
			struct server_rpc m = dig_msg(false, 1000 + k);
			assert(svin_rpc_send(&m));
		}
		assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE);

		/* Ride intent A: accepted in its own reserve slot despite full
		 * ordinary backlog. Entity 100, epoch 5, seq 1, mount. */
		struct server_rpc intent_a
			= ride_intent_msg(100, 5, 1, true);
		assert(svin_rpc_send(&intent_a));
		assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE + 1);

		/* Same-epoch same-seq retry: returns true, does not replace. */
		struct server_rpc intent_retry
			= ride_intent_msg(100, 5, 1, true);
		assert(svin_rpc_send(&intent_retry));
		assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE + 1);

		/* A mount for a different target is a distinct ownership action even
		 * within the same input epoch. It must not replace entity 100 at that
		 * request's earlier FIFO position: doing so strands entity 100's optimistic
		 * client occupancy forever because the server never sees the mount and can
		 * never publish its correction. With the sole ride reserve occupied, reject
		 * entity 200 for now so the caller retains send_dirty and retries after
		 * entity 100 drains. */
		uint32_t retarget_drops = svin_dropped_count();
		struct server_rpc intent_other_target
			= ride_intent_msg(200, 5, 3, true);
		assert(!svin_rpc_send(&intent_other_target));
		assert(svin_dropped_count() == retarget_drops + 1);
		assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE + 1);

		/* Same-epoch newer-seq: replaces. desired=false (dismount). */
		struct server_rpc intent_b
			= ride_intent_msg(100, 5, 2, false);
		assert(svin_rpc_send(&intent_b));
		assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE + 1);

		/* Same-epoch older-seq for the same target: returns true, does not
		 * replace. */
		struct server_rpc intent_old
			= ride_intent_msg(100, 5, 1, true);
		assert(svin_rpc_send(&intent_old));
		assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE + 1);

		/* Cross-epoch: reserve is full (epoch-5 occupies the one
		 * ride-intent slot). A cross-epoch intent must NOT become a
		 * second pending entry -- that would permit a cross-generation
		 * FIFO violation. svin_rpc_send returns false; pending count
		 * unchanged at 64+1; caller retains send_dirty. */
		struct server_rpc intent_xep
			= ride_intent_msg(100, 9, 1, true);
		assert(!svin_rpc_send(&intent_xep));
		assert(svin_pending_count() == SVIN_ACTION_PENDING_SIZE + 1);

		/* Flush epoch-5: drain the inbox + pending, then verify the
		 * epoch-5 dismount (seq 2, desired=false) is delivered. */
		drain();
		while(svin_pending_count() > 0) {
			svin_rpc_flush_pending();
			drain();
		}
		bool found_ep5 = false;
		for(size_t k = 0; k < received_count; k++) {
			if(received[k].type == SRPC_MINECART_RIDE_INTENT
			   && received[k].payload.minecart_ride_intent.input_epoch == 5) {
				found_ep5 = true;
				assert(received[k].payload.minecart_ride_intent.request_seq == 2);
				assert(!received[k].payload.minecart_ride_intent.desired_occupied);
			}
		}
		assert(found_ep5 && "epoch-5 ride-intent must be delivered");

		/* Now that the reserve is flushed, retry the same-epoch different target.
		 * It is accepted and delivered as its own action rather than having
		 * replaced entity 100. */
		received_count = 0;
		assert(svin_rpc_send(&intent_other_target));
		drain();
		while(svin_pending_count() > 0) {
			svin_rpc_flush_pending();
			drain();
		}
		bool found_other_target = false;
		for(size_t k = 0; k < received_count; k++) {
			if(received[k].type == SRPC_MINECART_RIDE_INTENT
			   && received[k].payload.minecart_ride_intent.entity_id == 200
			   && received[k].payload.minecart_ride_intent.input_epoch == 5
			   && received[k].payload.minecart_ride_intent.request_seq == 3)
				found_other_target = true;
		}
		assert(found_other_target
			   && "same-epoch retarget must be delivered after the first target");

		/* The later cross-epoch retry is also accepted and delivered. */
		received_count = 0;
		assert(svin_rpc_send(&intent_xep));
		drain();
		while(svin_pending_count() > 0) {
			svin_rpc_flush_pending();
			drain();
		}
		bool found_ep9 = false;
		for(size_t k = 0; k < received_count; k++) {
			if(received[k].type == SRPC_MINECART_RIDE_INTENT
			   && received[k].payload.minecart_ride_intent.input_epoch == 9) {
				found_ep9 = true;
			}
		}
		assert(found_ep9 && "epoch-9 ride-intent must be delivered after flush");
	}

	/* =====================================================================
	 * 8. EXACT A->B RETARGET: keep A's original mount (do not coalesce it into
	 * a later same-target state), reject B while A owns the reserve, then prove
	 * A reaches the server before B can retry. This is the client prediction
	 * failure shape: if B replaced A, authority could never publish A's release.
	 * ===================================================================== */
	{
		received_count = 0;
		assert(svin_pending_count() == 0
			   && "prior scenario must leave the shared channel drained");
		for(int k = 0; k < RPC_INBOX_SIZE; k++) {
			struct server_rpc m = pos_msg();
			assert(svin_rpc_send(&m));
		}

		struct server_rpc mount_a = ride_intent_msg(100, 12, 1, true);
		struct server_rpc mount_b = ride_intent_msg(200, 12, 2, true);
		assert(svin_rpc_send(&mount_a));
		assert(!svin_rpc_send(&mount_b));
		assert(svin_pending_count() == 1);

		drain();
		while(svin_pending_count() > 0) {
			svin_rpc_flush_pending();
			drain();
		}
		bool saw_a = false;
		bool saw_b = false;
		for(size_t k = 0; k < received_count; k++) {
			if(received[k].type != SRPC_MINECART_RIDE_INTENT)
				continue;
			saw_a |= received[k].payload.minecart_ride_intent.entity_id == 100
				&& received[k].payload.minecart_ride_intent.desired_occupied;
			saw_b |= received[k].payload.minecart_ride_intent.entity_id == 200;
		}
		assert(saw_a && !saw_b
			   && "A must reach authority before a different target retries");

		received_count = 0;
		assert(svin_rpc_send(&mount_b));
		drain();
		assert(received_count == 1
			   && received[0].type == SRPC_MINECART_RIDE_INTENT
			   && received[0].payload.minecart_ride_intent.entity_id == 200
			   && received[0].payload.minecart_ride_intent.desired_occupied);
	}

	test_guest_queue();
	{ /* CW_F1_CASE=host|both|pause|leave runs one F1 case (RED receipts) */
		const char* only = getenv("CW_F1_CASE");
		f1_remote_unchanged();
		if(!only || !strcmp(only, "host")) {
			static const int f1_counts[] = {32, 33, 34, 40, 101, 250};
			for(size_t k = 0; k < sizeof f1_counts / sizeof f1_counts[0]; k++)
				f1_lossless_host(f1_counts[k]);
		}
		if(!only || !strcmp(only, "both"))
			f1_lossless_both();
		if(!only || !strcmp(only, "pause"))
			f1_pause_progress();
		if(!only || !strcmp(only, "leave"))
			f1_leave_progress();
		if(!only || !strcmp(only, "credit"))
			f1_credit_accounting();
	}
	printf("test_svin_reliable: OK\n");
	return 0;
}
