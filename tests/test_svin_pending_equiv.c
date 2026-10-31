/* Copyright (c) 2026 CaveWii_v2 contributors
 * R-A8 A8-L10: SVIN_PENDING_SIZE grew by one for EVERY class when
 * SVIN_SPLIT_SET_RESERVE was added (source/network/server_interface.c:79-98).
 * That constant is the global `svin_pending_len >= SVIN_PENDING_SIZE` cutoff
 * used by every class AND the ring's modulus, so it is the one always-on
 * structural change in the live-split slice -- and the slice shipped without a
 * transport equivalence proof. This is that proof.
 *
 * WHY A PRE-CHANGE FIXTURE IS IMPOSSIBLE, and what is asserted instead. The
 * capacities are compile-time `#define`s inside server_interface.c; a test
 * cannot link two builds of that translation unit at once, so the pre-change
 * behaviour cannot be RUN side by side. What CAN be run -- and is, below -- is
 * every decision the pre-change code would have made, because those decisions
 * are fully determined by constants this file records verbatim from the
 * pre-change source. The claim being proved is therefore stated exactly:
 *
 *   For every class that existed BEFORE the change, the accept/reject decision
 *   and the drain order are identical to the pre-change constants. The one
 *   behavioural difference is that SRPC_SPLIT_SET has a slot of its own, and
 *   that slot is reachable by NOTHING ELSE.
 *
 * The mechanism behind it: the per-class capacities sum EXACTLY to
 * SVIN_PENDING_SIZE (64 + 3 + 1 + 1 + 1 + 1 + 1 + 1 [+ 1]), so the global
 * `svin_pending_len >= SVIN_PENDING_SIZE` term can only be true when every
 * class is already at its own capacity -- i.e. the per-class reserve always
 * binds first and the global term never decides anything. Growing the global
 * constant alongside a new per-class reserve therefore cannot move any other
 * class's decision. Case (a) measures that per class rather than asserting it:
 * at every rejection it also records that the queue was NOT full, so the
 * rejection demonstrably came from the class's own reserve.
 *
 * The ring's modulus is the second half of the finding. Insertion is at
 * (head + len) % SIZE and the drain is from head, so FIFO holds for any
 * modulus >= the live length -- case (c) drains a completely full queue and
 * case (d) forces head to WRAP past the end and drains again, both asserting
 * byte-for-byte identical order to the send order.
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "network/server_interface.h"

/* ---- the PRE-CHANGE constants, copied verbatim from the source this diff
 * edited (server_interface.c @ the commit before SVIN_SPLIT_SET_RESERVE). Every
 * expectation below is written against THESE, never against the live macros --
 * that is what makes this an equivalence assertion and not a tautology. */
#define PRE_ACTION_PENDING_SIZE 64
#define PRE_LIFECYCLE_RESERVE 3
#define PRE_GUEST_CLOSE_RESERVE 1
#define PRE_INPUT_ACK_RESERVE 1
#define PRE_STREAM_RECOVER_RESERVE 1
#define PRE_PLAYER_STATE_RESERVE 1
#define PRE_MINECART_CONTROL_RESERVE 1
#define PRE_RIDE_INTENT_RESERVE 1
#define PRE_PENDING_SIZE                                                       \
	(PRE_ACTION_PENDING_SIZE + PRE_LIFECYCLE_RESERVE + PRE_GUEST_CLOSE_RESERVE \
	 + PRE_INPUT_ACK_RESERVE + PRE_STREAM_RECOVER_RESERVE                      \
	 + PRE_PLAYER_STATE_RESERVE + PRE_MINECART_CONTROL_RESERVE                 \
	 + PRE_RIDE_INTENT_RESERVE) /* 73 */
/* ...and the one thing the live-split change added. */
#define POST_SPLIT_SET_RESERVE 1
/* S1-a2 added a SECOND additive control slot for the same reason: the guest
 * needs a PLAYER_STATE slot of its own or whichever owner reached the single
 * shared one first would starve the other (Astra gate-2 D4). Recorded here so
 * the modulus every order assertion below is written against stays exact --
 * the ACTION class total (PRE_ACTION_PENDING_SIZE) is untouched, which is the
 * "bounded total unchanged" half of D4; case (e) measures the per-owner cap
 * that lives INSIDE it. */
#define POST_GUEST_PLAYER_STATE_RESERVE 1
#define POST_PENDING_SIZE 	(PRE_PENDING_SIZE + POST_SPLIT_SET_RESERVE 	 + POST_GUEST_PLAYER_STATE_RESERVE) /* 75 */

/* The inbox (RPC_INBOX_SIZE) is what a send lands in while it has room; only a
 * FULL inbox pushes anything onto the pending queue, which is what this test is
 * about. 16 is server_interface.c's own constant; it is measured here rather
 * than assumed (fill_inbox asserts the count it actually took). */
#define INBOX_SIZE 16

/* ---- observed drain order ------------------------------------------------
 * Three uint32_t and nothing else: the order comparison below is a memcmp over
 * whole arrays, and a struct with interior padding would compare unspecified
 * bytes. */
struct mark {
	uint32_t type;
	uint32_t slot;
	uint32_t tag; /* unique per ordinary action; class-specific otherwise */
};
#define ORDER_MAX 256
static struct mark sent_order[ORDER_MAX];
static unsigned sent_n;
static struct mark seen_order[ORDER_MAX];
static unsigned seen_n;

static uint32_t rpc_tag(const struct server_rpc* call) {
	switch(call->type) {
		case SRPC_HOTBAR_SLOT: return (uint32_t)call->payload.hotbar_slot.slot;
		case SRPC_INPUT_READY: return call->payload.input_ready.epoch;
		case SRPC_STREAM_RECOVER: return call->payload.stream_recover.epoch;
		case SRPC_MINECART_CONTROL:
			return call->payload.minecart_control.entity_id;
		case SRPC_MINECART_RIDE_INTENT:
			return call->payload.minecart_ride_intent.entity_id;
		case SRPC_SPLIT_SET: return call->payload.split_set.request_seq;
		case SRPC_WINDOW_CLOSE: return call->payload.window_close.window;
		default: return 0;
	}
}
static void observe(struct server_rpc_source source, struct server_rpc* call,
					void* user) {
	(void)user;
	assert(seen_n < ORDER_MAX);
	seen_order[seen_n++]
		= (struct mark) {(uint32_t)call->type, source.slot, rpc_tag(call)};
}

/* ---- send helpers -------------------------------------------------------- */
static bool send(struct server_rpc_source source, struct server_rpc* call) {
	bool ok = svin_rpc_send_from(source, call);
	if(ok) {
		assert(sent_n < ORDER_MAX);
		sent_order[sent_n++]
			= (struct mark) {(uint32_t)call->type, source.slot, rpc_tag(call)};
	}
	return ok;
}
static const struct server_rpc_source HOST = {0};
static const struct server_rpc_source GUEST = {.slot = 1, .generation = 7};

static struct server_rpc ordinary(uint32_t tag) {
	struct server_rpc call = {.type = SRPC_HOTBAR_SLOT};
	call.payload.hotbar_slot.slot = tag;
	return call;
}
static struct server_rpc lifecycle_host(void) {
	return (struct server_rpc) {.type = SRPC_UNLOAD_WORLD};
}
static struct server_rpc lifecycle_guest(uint8_t window) {
	struct server_rpc call = {.type = SRPC_WINDOW_CLOSE};
	call.payload.window_close.window = window;
	return call;
}
static struct server_rpc input_ready(uint32_t epoch) {
	struct server_rpc call = {.type = SRPC_INPUT_READY};
	call.payload.input_ready.epoch = epoch;
	return call;
}
static struct server_rpc stream_recover(uint32_t epoch) {
	struct server_rpc call = {.type = SRPC_STREAM_RECOVER};
	call.payload.stream_recover.epoch = epoch;
	return call;
}
static struct server_rpc player_state(uint32_t seq) {
	struct server_rpc call = {.type = SRPC_PLAYER_STATE};
	call.payload.player_state.correction_seq = seq;
	return call;
}
static struct server_rpc minecart_control(uint32_t entity) {
	struct server_rpc call = {.type = SRPC_MINECART_CONTROL};
	call.payload.minecart_control.entity_id = entity;
	call.payload.minecart_control.ride_epoch = 1;
	call.payload.minecart_control.input_seq = 1;
	return call;
}
static struct server_rpc ride_intent(uint32_t entity) {
	struct server_rpc call = {.type = SRPC_MINECART_RIDE_INTENT};
	call.payload.minecart_ride_intent.entity_id = entity;
	call.payload.minecart_ride_intent.input_epoch = 1;
	call.payload.minecart_ride_intent.request_seq = 1;
	return call;
}
static struct server_rpc split_set(uint32_t generation, uint32_t seq) {
	struct server_rpc call = {.type = SRPC_SPLIT_SET};
	call.payload.split_set.enabled = true;
	call.payload.split_set.world_generation = generation;
	call.payload.split_set.request_seq = seq;
	return call;
}

/* Drain everything: process the inbox, let the pending queue refill it, repeat
 * until both are empty. The same two calls clin_update/the server thread make. */
static void drain_all(void) {
	for(unsigned guard = 0; guard < 64; guard++) {
		unsigned before = seen_n;
		svin_rpc_flush_pending();
		svin_process_owned_messages(observe, NULL, false);
		if(svin_pending_count() == 0 && seen_n == before)
			return;
	}
	assert(0 && "drain did not converge");
}
/* A clean transport: nothing pending, nothing in the inbox, order logs reset. */
static void reset_queue(void) {
	drain_all();
	assert(svin_pending_count() == 0);
	sent_n = seen_n = 0;
}
/* Fill the inbox so every later send is forced onto the PENDING queue. */
static void fill_inbox(uint32_t base) {
	for(unsigned i = 0; i < INBOX_SIZE; i++) {
		struct server_rpc call = ordinary(base + i);
		assert(send(HOST, &call));
		assert(svin_pending_count() == 0); /* still room in the inbox */
	}
	/* One more proves the inbox is genuinely full: it lands in pending. */
	struct server_rpc probe = ordinary(base + INBOX_SIZE);
	assert(send(HOST, &probe));
	assert(svin_pending_count() == 1);
}

/* ======================================================================== */
/* (a) Every PRE-CHANGE class still accepts exactly its pre-change capacity and
 * rejects the next one -- and at that rejection the queue is NOT full, so the
 * decision demonstrably came from the class's own reserve and not from the
 * global cutoff the +1 moved. `extra` is a send that must NOT consume a second
 * slot (a coalescing duplicate) where the class has one, NULL otherwise. */
static void class_capacity(const char* name, unsigned reserve,
						   struct server_rpc_source source,
						   struct server_rpc (*make)(uint32_t),
						   uint32_t first_tag, bool coalesces_duplicate) {
	reset_queue();
	fill_inbox(1000);
	size_t base = svin_pending_count();

	for(unsigned i = 0; i < reserve; i++) {
		struct server_rpc call = make(first_tag + i);
		assert(send(source, &call));
		assert(svin_pending_count() == base + i + 1);
	}
	if(coalesces_duplicate) {
		/* The class folds a newer duplicate into the pending one instead of
		 * consuming the reserve twice: accepted, queue unchanged. Unchanged by
		 * the +1 -- the coalescer runs before any capacity test. */
		struct server_rpc dup = make(first_tag);
		assert(svin_rpc_send_from(source, &dup));
		assert(svin_pending_count() == base + reserve);
	}
	/* One past the PRE-CHANGE reserve: rejected, and counted as a drop. */
	uint32_t drops = svin_dropped_count();
	struct server_rpc over = make(first_tag + reserve + 100);
	assert(!svin_rpc_send_from(source, &over));
	assert(svin_dropped_count() == drops + 1);
	assert(svin_pending_count() == base + reserve);
	/* THE equivalence point: the queue had room to spare, so the global
	 * `svin_pending_len >= SVIN_PENDING_SIZE` term -- the only thing the +1
	 * moved -- played no part. The pre-change build made this same decision. */
	assert(svin_pending_count() < (size_t)PRE_PENDING_SIZE);
	printf("  %-18s capacity %2u, rejected the next at len %2u (< %d)\n", name,
		   reserve, (unsigned)svin_pending_count(), PRE_PENDING_SIZE);
}

static struct server_rpc make_lifecycle_host(uint32_t t) {
	(void)t;
	return lifecycle_host();
}
static struct server_rpc make_lifecycle_guest(uint32_t t) {
	return lifecycle_guest((uint8_t)t);
}

static void a_per_class_capacity_unchanged(void) {
	puts("(a) per-class capacities, measured against the PRE-CHANGE constants:");
	/* The ordinary-action class: 64, and its own count is what binds. The first
	 * INBOX_SIZE+1 sends already put one ordinary action in the queue, so the
	 * class admits 63 more here -- fill_inbox's probe is the 64th. */
	reset_queue();
	fill_inbox(2000);
	for(unsigned i = 1; i < PRE_ACTION_PENDING_SIZE; i++) {
		struct server_rpc call = ordinary(3000 + i);
		assert(send(HOST, &call));
		assert(svin_pending_count() == i + 1);
	}
	assert(svin_pending_count() == PRE_ACTION_PENDING_SIZE);
	uint32_t drops = svin_dropped_count();
	struct server_rpc over = ordinary(9999);
	assert(!svin_rpc_send_from(HOST, &over));
	assert(svin_dropped_count() == drops + 1);
	assert(svin_pending_count() == PRE_ACTION_PENDING_SIZE);
	assert(svin_pending_count() < (size_t)PRE_PENDING_SIZE);
	printf("  %-18s capacity %2u, rejected the next at len %2u (< %d)\n",
		   "ordinary action", (unsigned)PRE_ACTION_PENDING_SIZE,
		   (unsigned)svin_pending_count(), PRE_PENDING_SIZE);

	class_capacity("lifecycle slot 0", PRE_LIFECYCLE_RESERVE, HOST,
				   make_lifecycle_host, 0, false);
	class_capacity("guest close slot 1", PRE_GUEST_CLOSE_RESERVE, GUEST,
				   make_lifecycle_guest, 1, true);
	class_capacity("input ack", PRE_INPUT_ACK_RESERVE, HOST, input_ready, 1,
				   true);
	class_capacity("stream recover", PRE_STREAM_RECOVER_RESERVE, HOST,
				   stream_recover, 1, true);
	class_capacity("minecart control", PRE_MINECART_CONTROL_RESERVE, HOST,
				   minecart_control, 1, true);
	class_capacity("ride intent", PRE_RIDE_INTENT_RESERVE, HOST, ride_intent, 1,
				   true);
	/* PLAYER_STATE has no non-coalescing second send at all: the coalescer
	 * replaces ANY pending snapshot of this class regardless of payload, so its
	 * reserve of 1 can never be exceeded by a second request. Asserted as the
	 * behaviour rather than forced into the shape of the others. */
	reset_queue();
	fill_inbox(4000);
	size_t base = svin_pending_count();
	struct server_rpc ps = player_state(1);
	assert(send(HOST, &ps));
	assert(svin_pending_count() == base + PRE_PLAYER_STATE_RESERVE);
	struct server_rpc ps2 = player_state(2);
	assert(svin_rpc_send_from(HOST, &ps2)); /* coalesced, not queued */
	assert(svin_pending_count() == base + PRE_PLAYER_STATE_RESERVE);
	assert(svin_pending_count() < (size_t)PRE_PENDING_SIZE);
	printf("  %-18s capacity %2u, a second snapshot coalesces at len %2u\n",
		   "player state", (unsigned)PRE_PLAYER_STATE_RESERVE,
		   (unsigned)svin_pending_count());
	puts("PASS (a) per-class capacity: every pre-change class accepts exactly "
		 "its pre-change reserve and rejects the next one with the queue still "
		 "short of the PRE-CHANGE size 73 -- the global cutoff the +1 moved "
		 "never decided any of them");
}

/* Fill every PRE-CHANGE class to capacity: the queue then holds exactly the
 * pre-change SVIN_PENDING_SIZE. Returns with the inbox full and 73 pending. */
static void fill_every_pre_change_class(void) {
	reset_queue();
	fill_inbox(5000);
	for(unsigned i = 1; i < PRE_ACTION_PENDING_SIZE; i++) {
		struct server_rpc call = ordinary(6000 + i);
		assert(send(HOST, &call));
	}
	for(unsigned i = 0; i < PRE_LIFECYCLE_RESERVE; i++) {
		struct server_rpc call = lifecycle_host();
		assert(send(HOST, &call));
	}
	struct server_rpc gclose = lifecycle_guest(3);
	assert(send(GUEST, &gclose));
	struct server_rpc ir = input_ready(11);
	assert(send(HOST, &ir));
	struct server_rpc sr = stream_recover(12);
	assert(send(HOST, &sr));
	struct server_rpc ps = player_state(13);
	assert(send(HOST, &ps));
	struct server_rpc mc = minecart_control(14);
	assert(send(HOST, &mc));
	struct server_rpc ri = ride_intent(15);
	assert(send(HOST, &ri));
	assert(svin_pending_count() == (size_t)PRE_PENDING_SIZE);
}

/* (b) The ONLY behavioural difference: with every pre-change class at capacity
 * -- the state in which the pre-change build was completely full -- a
 * SRPC_SPLIT_SET still fits, and NOTHING ELSE does. The +1 is reachable by one
 * class only. */
static void b_split_reserve_is_additive_only(void) {
	fill_every_pre_change_class();
	/* Pre-change, the queue was FULL here and every further send was dropped.
	 * That is still true for every pre-change class... */
	uint32_t drops = svin_dropped_count();
	struct server_rpc over_ordinary = ordinary(7777);
	assert(!svin_rpc_send_from(HOST, &over_ordinary));
	struct server_rpc over_life = lifecycle_host();
	assert(!svin_rpc_send_from(HOST, &over_life));
	struct server_rpc over_guest = lifecycle_guest(9);
	assert(!svin_rpc_send_from(GUEST, &over_guest));
	struct server_rpc over_ir = input_ready(99);
	assert(!svin_rpc_send_from(HOST, &over_ir));
	struct server_rpc over_sr = stream_recover(99);
	assert(!svin_rpc_send_from(HOST, &over_sr));
	struct server_rpc over_mc = minecart_control(99);
	assert(!svin_rpc_send_from(HOST, &over_mc));
	struct server_rpc over_ri = ride_intent(99);
	assert(!svin_rpc_send_from(HOST, &over_ri));
	assert(svin_dropped_count() == drops + 7);
	assert(svin_pending_count() == (size_t)PRE_PENDING_SIZE);

	/* ...and the FIRST added slot takes exactly one SPLIT_SET. */
	struct server_rpc ss = split_set(1, 1);
	assert(send(HOST, &ss));
	assert(svin_pending_count()
		   == (size_t)(PRE_PENDING_SIZE + POST_SPLIT_SET_RESERVE));
	/* A newer request for the SAME generation coalesces into it (the reserve is
	 * not consumed twice); a second generation is a different decision and is
	 * rejected because the reserve is now full. */
	struct server_rpc ss_newer = split_set(1, 2);
	assert(svin_rpc_send_from(HOST, &ss_newer));
	assert(svin_pending_count()
		   == (size_t)(PRE_PENDING_SIZE + POST_SPLIT_SET_RESERVE));
	drops = svin_dropped_count();
	struct server_rpc ss_other = split_set(2, 3);
	assert(!svin_rpc_send_from(HOST, &ss_other));
	assert(svin_dropped_count() == drops + 1);
	assert(svin_pending_count()
		   == (size_t)(PRE_PENDING_SIZE + POST_SPLIT_SET_RESERVE));
	/* The coalescer replaced the payload in place, so the queued request is the
	 * NEWER seq at the ORIGINAL FIFO position. Record that for (c)'s diff. */
	assert(sent_n > 0
		   && sent_order[sent_n - 1].type == (uint32_t)SRPC_SPLIT_SET);
	sent_order[sent_n - 1].tag = 2;

	/* The SECOND added slot (S1-a2) is reachable by a slot-1 SRPC_PLAYER_STATE
	 * alone, and ONLY while the split accept class is open. With the class
	 * CLOSED -- the configuration everything above runs in -- it is refused at
	 * the queue boundary, so the queue tops out at the pre-S1-a2 size: the new
	 * slot cannot change any decision in a build without the switch. */
	drops = svin_dropped_count();
	struct server_rpc gstate_off = player_state(41);
	assert(!svin_rpc_send_from(GUEST, &gstate_off));
	assert(svin_pending_count()
		   == (size_t)(PRE_PENDING_SIZE + POST_SPLIT_SET_RESERVE));
	/* (Refused by the accept class, not by capacity: it is not even counted as
	 * a pressure drop.) */
	assert(svin_dropped_count() == drops);
	svin_set_split_accept(true);
	struct server_rpc gstate_on = player_state(42);
	assert(send(GUEST, &gstate_on));
	assert(svin_pending_count() == (size_t)POST_PENDING_SIZE);
	/* ...and it is the GUEST's own slot: the host's single PLAYER_STATE slot
	 * was already full above, and a second guest snapshot coalesces into this
	 * one rather than consuming the reserve twice. */
	struct server_rpc gstate_newer = player_state(43);
	assert(svin_rpc_send_from(GUEST, &gstate_newer));
	assert(svin_pending_count() == (size_t)POST_PENDING_SIZE);
	assert(sent_n > 0
		   && sent_order[sent_n - 1].type == (uint32_t)SRPC_PLAYER_STATE);
	sent_order[sent_n - 1].tag = rpc_tag(&gstate_newer);
	svin_set_split_accept(false);
	printf("PASS (b) both added slots are additive only: with all %d "
		   "pre-change slots full, 7 further sends across every pre-change "
		   "class are still rejected; only SRPC_SPLIT_SET reaches slot %d and "
		   "only a slot-1 SRPC_PLAYER_STATE (split class open) reaches slot "
		   "%d\n",
		   PRE_PENDING_SIZE, PRE_PENDING_SIZE + POST_SPLIT_SET_RESERVE,
		   POST_PENDING_SIZE);
}

/* (c) Drain order across a COMPLETELY FULL queue is byte-for-byte the send
 * order. This is the half of A8-L10 about the ring's modulus: insertion at
 * (head + len) % SIZE with a drain from head is FIFO for any modulus that
 * covers the live length, and here the live length IS the modulus. */
static void c_drain_order_fifo(void) {
	assert(svin_pending_count() == (size_t)POST_PENDING_SIZE);
	unsigned expect = sent_n;
	seen_n = 0;
	drain_all();
	assert(svin_pending_count() == 0);
	assert(seen_n == expect);
	assert(!memcmp(seen_order, sent_order, expect * sizeof(struct mark)));
	printf("PASS (c) drain order: %u messages (inbox %d + a full %d-deep "
		   "pending queue) drained in exactly the order they were sent\n",
		   expect, INBOX_SIZE, POST_PENDING_SIZE);
}

/* (d) ...and again with the head WRAPPED past the end of the ring, which is the
 * only state in which a changed modulus could reorder or alias anything. */
static void d_drain_order_after_wrap(void) {
	reset_queue();
	fill_inbox(8000);
	/* Push most of a full queue, then drain it: head advances to ~2/3 of the
	 * ring. The next fill's insertions wrap around the end. */
	for(unsigned i = 1; i < 50; i++) {
		struct server_rpc call = ordinary(8100 + i);
		assert(send(HOST, &call));
	}
	drain_all();
	assert(svin_pending_count() == 0);

	sent_n = seen_n = 0;
	fill_inbox(8200);
	for(unsigned i = 1; i < PRE_ACTION_PENDING_SIZE; i++) {
		struct server_rpc call = ordinary(8300 + i);
		assert(send(HOST, &call));
	}
	struct server_rpc ss = split_set(5, 5);
	assert(send(HOST, &ss));
	assert(svin_pending_count() == (size_t)PRE_ACTION_PENDING_SIZE + 1);
	unsigned expect = sent_n;
	seen_n = 0;
	drain_all();
	assert(seen_n == expect);
	assert(!memcmp(seen_order, sent_order, expect * sizeof(struct mark)));
	printf("PASS (d) drain order after a ring WRAP: %u messages drained in "
		   "send order with the head wrapped past the %d-entry modulus\n",
		   expect, POST_PENDING_SIZE);
}

/* ======================================================================== */
/* (e) S1-a2 / Astra gate-2 D4: the PER-OWNER action reserve.
 *
 * Four claims, each measured rather than argued:
 *  (e1) With the split accept class CLOSED -- the shipped default and the
 *       configuration cases (a)-(d) above measure -- the HOST still gets the
 *       full pre-change 64-deep ordinary class. This is the "host-only queue
 *       behaviour byte-identical" requirement: the reserve is invisible until
 *       contention is possible.
 *  (e2) With the class OPEN, a HOST flood cannot starve the guest: the host is
 *       capped at 64 - SVIN_ACTION_OWNER_RESERVE and the guest is still
 *       admitted afterwards.
 *  (e3) ...and symmetrically a GUEST flood cannot starve the host.
 *  (e4) Per-owner FIFO order survives an interleaved drain, and a departed
 *       session's entries are dropped -- releasing its reservation -- by
 *       svin_rpc_drop_owner, without disturbing the other owner's order.
 */
#define OWNER_RESERVE 16 /* SVIN_ACTION_OWNER_RESERVE, server_interface.c */
static void e_per_owner_action_reserve(void) {
	const struct server_rpc_source guest = {.slot = 1, .generation = 9};
	const struct server_rpc_source guest2 = {.slot = 1, .generation = 10};

	/* ---- (e1) switch OFF: the host keeps all 64 --------------------------- */
	svin_set_split_accept(false);
	reset_queue();
	fill_inbox(6000);
	unsigned accepted = 1; /* fill_inbox already queued one */
	for(;;) {
		struct server_rpc call = ordinary(6100 + accepted);
		if(!send(HOST, &call))
			break;
		accepted++;
		assert(accepted <= PRE_ACTION_PENDING_SIZE + 1);
	}
	assert(accepted == PRE_ACTION_PENDING_SIZE);
	assert(svin_pending_count() == PRE_ACTION_PENDING_SIZE);
	reset_queue();

	/* ---- (e2) switch ON: a host flood leaves the guest a way in ----------- */
	svin_set_split_accept(true);
	reset_queue();
	fill_inbox(7000);
	accepted = 1;
	for(;;) {
		struct server_rpc call = ordinary(7100 + accepted);
		if(!send(HOST, &call))
			break;
		accepted++;
		assert(accepted <= PRE_ACTION_PENDING_SIZE + 1);
	}
	assert(accepted == PRE_ACTION_PENDING_SIZE - OWNER_RESERVE);
	/* The queue is NOT full, so the rejection came from the OWNER cap. */
	assert(svin_pending_count() < POST_PENDING_SIZE);
	{   /* ...and the guest is admitted, up to its own share. */
		unsigned gaccepted = 0;
		for(;;) {
			struct server_rpc call = ordinary(7500 + gaccepted);
			if(!send(guest, &call))
				break;
			gaccepted++;
			assert(gaccepted <= OWNER_RESERVE + 1);
		}
		assert(gaccepted == OWNER_RESERVE);
	}
	reset_queue();

	/* ---- (e3) ...and symmetrically ---------------------------------------- */
	fill_inbox(8000); /* the inbox filler is host traffic; drain it first */
	drain_all();
	sent_n = seen_n = 0;
	fill_inbox(8100);
	drain_all();
	sent_n = seen_n = 0;
	/* Re-fill the inbox, then flood from the GUEST. */
	fill_inbox(8200);
	{
		unsigned gaccepted = 0;
		for(;;) {
			struct server_rpc call = ordinary(8300 + gaccepted);
			if(!send(guest, &call))
				break;
			gaccepted++;
			assert(gaccepted <= PRE_ACTION_PENDING_SIZE + 1);
		}
		assert(gaccepted == PRE_ACTION_PENDING_SIZE - OWNER_RESERVE);
		/* The host (which already holds the one fill_inbox overflow entry) can
		 * still get in: its reserve held. */
		unsigned haccepted = 0;
		for(;;) {
			struct server_rpc call = ordinary(8800 + haccepted);
			if(!send(HOST, &call))
				break;
			haccepted++;
			assert(haccepted <= OWNER_RESERVE + 1);
		}
		assert(haccepted > 0);
	}
	reset_queue();

	/* ---- (e4) per-owner FIFO + teardown release --------------------------- */
	fill_inbox(9000);
	struct server_rpc a = ordinary(9101), b = ordinary(9102),
					  c = ordinary(9103), d = ordinary(9104);
	assert(send(guest, &a));
	assert(send(HOST, &b));
	assert(send(guest, &c));
	assert(send(guest2, &d)); /* a DIFFERENT session */
	/* Drop exactly one session: its two entries go, both other entries stay in
	 * their original relative order. */
	size_t before = svin_pending_count();
	assert(svin_rpc_drop_owner(1, guest.generation) == 2);
	assert(svin_pending_count() == before - 2);
	assert(svin_rpc_drop_owner(0, 0) == 0); /* the host is never droppable */
	/* Expected drain = everything SENT so far minus the two dropped tags, in
	 * send order (so the surviving owners keep their relative FIFO). */
	unsigned expect_n = 0;
	static struct mark expect[ORDER_MAX];
	for(unsigned i = 0; i < sent_n; i++)
		if(sent_order[i].tag != 9101 && sent_order[i].tag != 9103)
			expect[expect_n++] = sent_order[i];
	seen_n = 0;
	drain_all();
	assert(seen_n == expect_n);
	for(unsigned i = 0; i < seen_n; i++) {
		assert(seen_order[i].type == expect[i].type);
		assert(seen_order[i].slot == expect[i].slot);
		assert(seen_order[i].tag == expect[i].tag);
	}
	/* The dropped session released its reservation: a fresh flood from the
	 * same slot gets its full share again. */
	reset_queue();
	fill_inbox(9500);
	{
		unsigned gaccepted = 0;
		for(;;) {
			struct server_rpc call = ordinary(9600 + gaccepted);
			if(!send(guest2, &call))
				break;
			gaccepted++;
			assert(gaccepted <= PRE_ACTION_PENDING_SIZE + 1);
		}
		assert(gaccepted == PRE_ACTION_PENDING_SIZE - OWNER_RESERVE);
	}

	/* ---- (e5) the teardown drop KEEPS the queued WINDOW_CLOSE ------------
	 * R-S1a2 F5: WINDOW_CLOSE is the container-ownership RELEASE, and the
	 * control class carries a dedicated SVIN_GUEST_CLOSE_RESERVE slot so a
	 * departing guest can always queue one. Dropping it with the rest of that
	 * session traffic silently broke that reserve contract. */
	reset_queue();
	fill_inbox(9700);
	struct server_rpc gclose = lifecycle_guest(3);
	struct server_rpc ga = ordinary(9801), gb = ordinary(9802),
					  hc = ordinary(9803);
	assert(send(guest, &ga));
	assert(send(guest, &gclose));
	assert(send(guest, &gb));
	assert(send(HOST, &hc));
	/* Only the two ORDINARY guest entries go; the close and the host entry
	 * stay, in their original relative order. */
	size_t close_before = svin_pending_count();
	assert(svin_rpc_drop_owner(1, guest.generation) == 2);
	assert(svin_pending_count() == close_before - 2);
	expect_n = 0;
	for(unsigned i = 0; i < sent_n; i++)
		if(sent_order[i].tag != 9801 && sent_order[i].tag != 9802)
			expect[expect_n++] = sent_order[i];
	seen_n = 0;
	drain_all();
	assert(seen_n == expect_n);
	{
		bool saw_close = false;
		for(unsigned i = 0; i < seen_n; i++) {
			assert(seen_order[i].type == expect[i].type);
			assert(seen_order[i].slot == expect[i].slot);
			assert(seen_order[i].tag == expect[i].tag);
			if(seen_order[i].type == (uint32_t)SRPC_WINDOW_CLOSE
			   && seen_order[i].slot == 1 && seen_order[i].tag == 3)
				saw_close = true;
		}
		assert(saw_close); /* the guest close survived the drop and dispatched */
	}
	reset_queue();
	svin_set_split_accept(false);
	printf("PASS (e) per-owner action reserve: host keeps %d with the split"
		   " class closed; with it open neither owner exceeds %d, the other"
		   " owner still admitted both ways, per-owner FIFO preserved and"
		   " svin_rpc_drop_owner releases a departed session\n",
		   PRE_ACTION_PENDING_SIZE, PRE_ACTION_PENDING_SIZE - OWNER_RESERVE);
}

/* (f) S3 golden: an admitted guest's existing SRPC_RESPAWN verb belongs to
 * the same reliable ordinary FIFO as its other gameplay actions. No future
 * readiness payload is assumed here; this pins only the existing envelope. */
static void f_guest_respawn_ordinary_fifo(void) {
	svin_set_split_accept(true);
	reset_queue();
	struct server_rpc before = ordinary(81);
	struct server_rpc respawn = {.type = SRPC_RESPAWN,
		.payload.respawn = {.owner = 1, .generation = GUEST.generation}};
	struct server_rpc after = ordinary(82);
	assert(send(GUEST, &before));
	assert(send(GUEST, &respawn)
		   && "slot-1 SRPC_RESPAWN must enter the split ordinary class");
	assert(send(GUEST, &after));
	drain_all();
	assert(seen_n == 3);
	assert(seen_order[0].type == SRPC_HOTBAR_SLOT
		   && seen_order[0].slot == 1 && seen_order[0].tag == 81);
	assert(seen_order[1].type == SRPC_RESPAWN && seen_order[1].slot == 1);
	assert(seen_order[2].type == SRPC_HOTBAR_SLOT
		   && seen_order[2].slot == 1 && seen_order[2].tag == 82);
	reset_queue();
	svin_set_split_accept(false);
	puts("PASS (f) guest respawn uses the reliable split ordinary FIFO");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(svin_init());
	/* The live-split accept class plays no part here: every message this test
	 * sends is one svin_rpc_send_from admits with the class OFF (a slot-1
	 * SRPC_PLAYER_POS is the only thing that class gates). Closed explicitly so
	 * the queue behaviour is measured in the same configuration a build without
	 * the switch would run in. */
	svin_set_split_accept(false);

	a_per_class_capacity_unchanged();
	b_split_reserve_is_additive_only();
	c_drain_order_fifo();
	d_drain_order_after_wrap();
	e_per_owner_action_reserve();
	f_guest_respawn_ordinary_fifo();

	reset_queue();
	printf("test_svin_pending_equiv: PASS (pre-change capacities %d/%d/%d/%d/"
		   "%d/%d/%d/%d = %d preserved; SVIN_SPLIT_SET_RESERVE adds slot %d, "
		   "reachable by SRPC_SPLIT_SET alone; FIFO drain order identical, "
		   "wrapped and unwrapped)\n",
		   PRE_ACTION_PENDING_SIZE, PRE_LIFECYCLE_RESERVE,
		   PRE_GUEST_CLOSE_RESERVE, PRE_INPUT_ACK_RESERVE,
		   PRE_STREAM_RECOVER_RESERVE, PRE_PLAYER_STATE_RESERVE,
		   PRE_MINECART_CONTROL_RESERVE, PRE_RIDE_INTENT_RESERVE,
		   PRE_PENDING_SIZE, POST_PENDING_SIZE);
	return 0;
}
