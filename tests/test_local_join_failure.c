/* Experimental local split-screen: every way a Join can FAIL, and the proof
 * that none of them leaves a half-admitted second player behind. Pairs with
 * test_local_join_ready (the success path). GPL-3.0-or-later. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "entity/entity.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "platform/input_inventory.h"
#include "platform/time.h"

static void feed(struct client_session* s, uint64_t ticket, uint64_t generation,
				 int result) {
	struct client_rpc call = {.type = CRPC_LOCAL_GUEST,
		.payload.local_guest
		= {.ticket = ticket, .generation = generation, .result = result}};
	client_session_local_status(s, &call);
}

static uint64_t attach(struct client_session* s, uint64_t generation) {
	assert(client_session_request_local_join(s));
	uint64_t ticket = s->local_ticket;
	feed(s, ticket, generation, LOCAL_GUEST_OFFER);
	feed(s, ticket, generation, LOCAL_GUEST_INSTALLED);
	feed(s, ticket, generation, LOCAL_GUEST_ATTACHED);
	assert(s->local_phase == CLIENT_GUEST_ATTACHED);
	s->local_pending = false;
	return ticket;
}

/* The server refuses the BODY while the guest's terrain is still streaming.
 * That denial must be retryable and must never retire the ATTACHED session --
 * the guest keeps its windows and can still Leave. */
static void test_denied_is_retryable(void) {
	struct client_session s = {0};
	struct entity body = {.type = ENTITY_LOCAL_PLAYER, .id = 3};
	uint64_t ticket = attach(&s, 71);
	for(int attempt = 0; attempt < 3; attempt++) {
		assert(client_session_request_local_gameplay(&s));
		s.local_pending = false;
		feed(&s, ticket, 71, LOCAL_GUEST_GAMEPLAY_DENIED);
		assert(!s.local_gameplay_ready);
		assert(s.local_phase == CLIENT_GUEST_ATTACHED && s.local_bound);
	}
	/* And the retry that finally lands is a normal grant. */
	assert(client_session_request_local_gameplay(&s));
	s.local_pending = false;
	s.local_player = &body;
	feed(&s, ticket, 71, LOCAL_GUEST_GAMEPLAY_READY);
	assert(s.local_gameplay_ready);
	/* A late DENIED for the same generation closes the gate again rather than
	 * leaving a body the server no longer simulates. */
	feed(&s, ticket, 71, LOCAL_GUEST_GAMEPLAY_DENIED);
	assert(!s.local_gameplay_ready);
	client_session_clear_windows(&s);
	puts("PASS denied: retryable, ATTACHED preserved, late denial closes the gate");
}

/* A rejected Join before any binding leaves a FAILED session that a later
 * press may retry -- and never a gameplay gate. */
static void test_rejected_join(void) {
	struct client_session s = {0};
	assert(client_session_request_local_join(&s));
	uint64_t ticket = s.local_ticket;
	feed(&s, ticket, 0, LOCAL_GUEST_REJECTED);
	assert(s.local_phase == CLIENT_GUEST_FAILED && !s.local_bound);
	assert(!s.local_gameplay_ready && !s.local_pending);
	assert(!client_session_request_local_gameplay(&s));
	/* FAILED is retryable by design (a second controller may press Join
	 * again); IDLE and FAILED are the only phases that accept one. */
	assert(client_session_request_local_join(&s));
	assert(s.local_phase == CLIENT_GUEST_REQUESTED);
	client_session_clear_windows(&s);
	puts("PASS rejected join: FAILED, no gate, retryable");
}

/* Leaving retires the gate before the acknowledgement even arrives, so a
 * frame drawn between the request and the reply can never draw two views for
 * a player the server is tearing down. */
static void test_leave_closes_the_gate(void) {
	struct client_session s = {0};
	struct entity body = {.type = ENTITY_LOCAL_PLAYER, .id = 5};
	uint64_t ticket = attach(&s, 83);
	assert(client_session_request_local_gameplay(&s));
	s.local_pending = false;
	s.local_player = &body;
	feed(&s, ticket, 83, LOCAL_GUEST_GAMEPLAY_READY);
	assert(s.local_gameplay_ready);
	assert(client_session_request_local_leave(&s));
	assert(s.local_phase == CLIENT_GUEST_LEAVING);
	feed(&s, ticket, 83, LOCAL_GUEST_LEFT);
	assert(!s.local_gameplay_ready && !s.local_bound
		   && s.local_phase == CLIENT_GUEST_IDLE);
	/* client_interface retires the BODY on the same edge (this fixture owns
	 * it directly); without that, a rejoin could not even be requested. */
	s.local_player = NULL;
	/* A failed server-side checkpoint keeps the session bound for a retry,
	 * but the gameplay gate stays shut. */
	uint64_t next = attach(&s, 84);
	assert(client_session_request_local_gameplay(&s));
	s.local_pending = false;
	feed(&s, next, 84, LOCAL_GUEST_GAMEPLAY_READY);
	assert(!s.local_gameplay_ready); /* the body was retired above */
	assert(client_session_request_local_leave(&s));
	feed(&s, next, 84, LOCAL_GUEST_LEAVE_FAILED);
	assert(s.local_phase == CLIENT_GUEST_FAILED && s.local_bound);
	assert(!s.local_gameplay_ready);
	client_session_clear_windows(&s);
	puts("PASS leave: gate shut on request, on LEFT and on LEAVE_FAILED");
}

/* No offered device, no join. The approved lifecycle only ever hands JOINING
 * to hardware the inventory actually DETECTED, and a refused admission
 * publishes "Could not join" instead of admitting anyone. */
static void test_no_device_no_join(void) {
	static struct input_inventory inv;
	static struct input_owner owners[INPUT_LOCAL_PLAYERS];
	const struct input_device gc = {INPUT_DEVICE_GAMECUBE, 0};
	const struct input_device other = {INPUT_DEVICE_GAMECUBE, 2};
	const struct input_identity pad = {true, INPUT_CAT_GAMECUBE};
	memset(&inv, 0, sizeof(inv));
	memset(owners, 0, sizeof(owners));

	/* Nothing detected yet: a request for a device the inventory never
	 * offered is refused, and the machine stays IDLE. */
	assert(!input_inventory_join_request(&inv, gc, (ptime_t) {0}));
	assert(inv.join.state == INPUT_JOIN_IDLE);

	for(unsigned ms = 0; ms <= INPUT_IDENTITY_STABLE_MS + 60; ms++)
		input_inventory_poll(&inv, gc, pad, true, true, true,
							 time_add_ms((ptime_t) {0}, ms), owners,
							 INPUT_LOCAL_PLAYERS);
	assert(inv.join.state == INPUT_JOIN_OFFERED);
	/* The offer belongs to ONE device; a different pad cannot claim it. */
	assert(!input_inventory_join_request(&inv, other,
		time_add_ms((ptime_t) {0}, 400)));
	assert(inv.join.state == INPUT_JOIN_OFFERED);

	assert(input_inventory_join_request(&inv, gc,
		time_add_ms((ptime_t) {0}, 400)));
	assert(inv.join.state == INPUT_JOIN_JOINING);
	/* A second request while one is in flight is refused. */
	assert(!input_inventory_join_request(&inv, gc,
		time_add_ms((ptime_t) {0}, 401)));
	input_inventory_join_settle(&inv, false, time_add_ms((ptime_t) {0}, 500));
	assert(inv.join.state == INPUT_JOIN_FAILED);
	/* A settle with nothing in flight is inert -- a stale success can never
	 * silently promote a failed join. */
	input_inventory_join_settle(&inv, true, time_add_ms((ptime_t) {0}, 600));
	assert(inv.join.state == INPUT_JOIN_FAILED);
	puts("PASS no offered device: an un-offered pad can never request a join, "
		 "and a stale settle cannot promote a failure");
}

int main(void) {
	memset(&gstate.players[1], 0, sizeof(gstate.players[1]));
	test_denied_is_retryable();
	test_rejected_join();
	test_leave_closes_the_gate();
	test_no_device_no_join();
	puts("test_local_join_failure: PASS");
	return 0;
}
