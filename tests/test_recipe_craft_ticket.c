/*
	Requirement 3 (lane/craft-rpc): idempotence, staleness, ownership.

	The selected-recipe request is ticketed. On the SERVER each actor retires at
	most one ticket: an exact retry (a resend after a dropped reply) replays the
	recorded verdict WITHOUT applying the batch a second time, an older ticket is
	refused as stale, and every rejection names a reason code. Ownership is taken
	from the queue envelope, never from the payload -- the payload's player and
	window are checked against the actor the envelope resolved, so a split-screen
	client cannot drive the other player's inventory even by forging both fields.

	On the CLIENT one request is in flight per session and only a reply echoing
	all three of ticket/player/window retires it; anything else is inert.

	This is the one lane test that drives the REAL RPC plumbing end to end --
	svin_rpc_send_from + server_local_test_process_messages for the guest path,
	so the split-screen rejection is proven through the actual dispatcher rather
	than by calling the handler directly.
*/

#ifndef CAVEWII_SERVER_LOCAL_TEST
#define CAVEWII_SERVER_LOCAL_TEST 1
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "block/blocks.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "platform/time.h"
#include "item/inventory.h"
#include "item/items.h"
#include "item/recipe.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"

/* ---------------------------------------------------------------- client -- */

struct sent {
	unsigned count;
	struct server_rpc last;
};
static bool capture_request(void* user, struct server_rpc* rpc) {
	struct sent* out = user;
	out->count++;
	out->last = *rpc;
	return true;
}
static bool refuse_request(void* user, struct server_rpc* rpc) {
	(void)rpc;
	(*(unsigned*)user)++;
	return false;
}

static uint16_t log_to_planks(void) {
	for(size_t n = 0; n < array_recipe_size(recipes_crafting); n++) {
		const struct recipe* r = array_recipe_cget(recipes_crafting, n);
		if(r->width == 1 && r->height == 1 && r->shape[0].item.id == BLOCK_LOG
		   && r->result.id == BLOCK_PLANKS)
			return (uint16_t)n;
	}
	assert(!"1x1 log -> planks recipe missing");
	return RECIPE_CRAFT_INDEX_NONE;
}

static void client_retires_only_its_own_reply(uint16_t recipe) {
	struct client_session session = {0};
	session.visible_window = WINDOWC_CRAFTING;
	/* Heap-allocated on purpose: this case ends by calling
	 * client_session_clear_windows(), which owns and frees the containers. */
	struct window_container* wc = malloc(sizeof(*wc));
	assert(wc && windowc_create(wc, WINDOW_TYPE_WORKBENCH, CRAFTING_SIZE));
	session.windows[WINDOWC_CRAFTING] = wc;

	struct sent sent = {0};
	assert(client_session_recipe_craft_request(&session, 0, WINDOWC_CRAFTING,
		recipe, capture_request, &sent));
	assert(sent.count == 1 && sent.last.type == SRPC_RECIPE_CRAFT);
	assert(sent.last.payload.recipe_craft.recipe_index == recipe);
	assert(sent.last.payload.recipe_craft.player == 0);
	assert(sent.last.payload.recipe_craft.window == WINDOWC_CRAFTING);
	uint32_t ticket = sent.last.payload.recipe_craft.ticket;
	assert(ticket != 0 && session.craft_pending);

	/* A second request while one is in flight is refused, unsent. */
	assert(!client_session_recipe_craft_request(&session, 0, WINDOWC_CRAFTING,
		recipe, capture_request, &sent));
	assert(sent.count == 1);

	struct client_rpc reply = {.type = CRPC_RECIPE_CRAFT,
		.payload.recipe_craft = {.ticket = ticket - 1, .player = 0,
			.window = WINDOWC_CRAFTING, .reason = RECIPE_CRAFT_OK}};
	/* Stale ticket: handled, but inert. */
	assert(client_session_window_reply(&session, &reply));
	assert(session.craft_pending && session.craft_reply_serial == 0);
	/* Right ticket, wrong window. */
	reply.payload.recipe_craft.ticket = ticket;
	reply.payload.recipe_craft.window = WINDOWC_INVENTORY;
	assert(client_session_window_reply(&session, &reply));
	assert(session.craft_pending && session.craft_reply_serial == 0);
	/* Right ticket and window, wrong player. */
	reply.payload.recipe_craft.window = WINDOWC_CRAFTING;
	reply.payload.recipe_craft.player = 1;
	assert(client_session_window_reply(&session, &reply));
	assert(session.craft_pending && session.craft_reply_serial == 0);
	/* All three match: retired, with the reason installed. */
	reply.payload.recipe_craft.player = 0;
	reply.payload.recipe_craft.reason = RECIPE_CRAFT_NO_CAPACITY;
	assert(client_session_window_reply(&session, &reply));
	assert(!session.craft_pending && session.craft_reply_serial == 1);
	assert(session.craft_reason == RECIPE_CRAFT_NO_CAPACITY);
	/* A duplicate of that same reply cannot re-fire the completion edge. */
	assert(client_session_window_reply(&session, &reply));
	assert(session.craft_reply_serial == 1);

	/* Tickets are monotonic across requests. */
	assert(client_session_recipe_craft_request(&session, 0, WINDOWC_CRAFTING,
		recipe, capture_request, &sent));
	assert(sent.last.payload.recipe_craft.ticket > ticket);

	/* Tearing the windows down retires the in-flight request rather than
	 * leaving the guide waiting on a reply that can never arrive. */
	client_session_clear_windows(&session);
	assert(!session.craft_pending);
	puts("  client: only a ticket+player+window match retires the request");
}

/* Liveness on the CLIENT (hostile review, finding 3). A reply that can never
 * arrive -- the window closed, was replaced, or the server answered into the
 * void -- must not leave the guide waiting forever. client_session_recipe_craft_
 * tick retires such a request locally and raises the same completion edge a real
 * reply would, so the UI un-blocks either way. */
static void client_retires_a_request_that_cannot_be_answered(uint16_t recipe) {
	struct sent sent = {0};
	struct window_container wc;

	/* (a) the window stopped being the visible one -- the player closed it. */
	struct client_session session = {0};
	assert(windowc_create(&wc, WINDOW_TYPE_WORKBENCH, CRAFTING_SIZE));
	session.windows[WINDOWC_CRAFTING] = &wc;
	session.visible_window = WINDOWC_CRAFTING;
	assert(client_session_recipe_craft_request(&session, 0, WINDOWC_CRAFTING,
		recipe, capture_request, &sent));
	assert(session.craft_pending);
	/* Still open and still inside the deadline: the tick leaves it alone. */
	client_session_recipe_craft_tick(&session, time_get());
	assert(session.craft_pending && session.craft_reply_serial == 0);
	session.visible_window = -1;
	client_session_recipe_craft_tick(&session, time_get());
	assert(!session.craft_pending && session.craft_reply_serial == 1);
	assert(session.craft_reason == RECIPE_CRAFT_TIMED_OUT);
	/* Idempotent: a second sweep raises no second completion edge. */
	client_session_recipe_craft_tick(&session, time_get());
	assert(session.craft_reply_serial == 1);

	/* (b) same routing index, NEW container -- a close/reopen. The reply for the
	 * old generation can never be installed against the new window. */
	session.visible_window = WINDOWC_CRAFTING;
	assert(client_session_recipe_craft_request(&session, 0, WINDOWC_CRAFTING,
		recipe, capture_request, &sent));
	wc.generation++;
	client_session_recipe_craft_tick(&session, time_get());
	assert(!session.craft_pending && session.craft_reply_serial == 2);
	wc.generation--;

	/* (c) the reply was simply lost: the deadline retires it. */
	assert(client_session_recipe_craft_request(&session, 0, WINDOWC_CRAFTING,
		recipe, capture_request, &sent));
	assert(!time_reached(time_get(), session.craft_deadline)); /* real patience */
	session.craft_deadline = time_get(); /* pretend it has elapsed */
	client_session_recipe_craft_tick(&session, time_add_ms(time_get(), 1));
	assert(!session.craft_pending && session.craft_reply_serial == 3);
	assert(session.craft_reason == RECIPE_CRAFT_TIMED_OUT);

	/* And a retired request frees the session to ask again. */
	assert(client_session_recipe_craft_request(&session, 0, WINDOWC_CRAFTING,
		recipe, capture_request, &sent));
	session.windows[WINDOWC_CRAFTING] = NULL;
	windowc_destroy(&wc);
	puts("  client: a request that can never be answered is retired locally");
}

/* The reply is dispatched to the local player its OWN payload names. A guest's
 * craft reply normally arrives generation-tagged and never reaches this switch;
 * the dropped-envelope answer above does, untagged, and landing it on player 0
 * would strand exactly the request it exists to release. */
static void reply_reaches_the_player_it_names(uint16_t recipe) {
	struct sent sent = {0};
	struct window_container host_wc, guest_wc;
	assert(windowc_create(&host_wc, WINDOW_TYPE_WORKBENCH, CRAFTING_SIZE));
	assert(windowc_create(&guest_wc, WINDOW_TYPE_WORKBENCH, CRAFTING_SIZE));

	memset(&gstate.players[0], 0, sizeof(gstate.players[0]));
	memset(&gstate.players[1], 0, sizeof(gstate.players[1]));
	gstate.players[0].windows[WINDOWC_CRAFTING] = &host_wc;
	gstate.players[0].visible_window = WINDOWC_CRAFTING;
	gstate.players[1].windows[WINDOWC_CRAFTING] = &guest_wc;
	gstate.players[1].visible_window = WINDOWC_CRAFTING;
	assert(client_session_recipe_craft_request(&gstate.players[0], 0,
		WINDOWC_CRAFTING, recipe, capture_request, &sent));
	uint32_t host_ticket = sent.last.payload.recipe_craft.ticket;
	assert(client_session_recipe_craft_request(&gstate.players[1], 1,
		WINDOWC_CRAFTING, recipe, capture_request, &sent));
	uint32_t guest_ticket = sent.last.payload.recipe_craft.ticket;
	assert(host_ticket != guest_ticket);

	struct client_rpc reply = {.type = CRPC_RECIPE_CRAFT,
		.payload.recipe_craft = {.ticket = guest_ticket, .player = 1,
			.window = WINDOWC_CRAFTING, .reason = RECIPE_CRAFT_STALE_SESSION}};
	clin_process(&reply);
	assert(!gstate.players[1].craft_pending);
	assert(gstate.players[1].craft_reason == RECIPE_CRAFT_STALE_SESSION);
	assert(gstate.players[1].craft_reply_serial == 1);
	/* P1's request is untouched -- it was never addressed. */
	assert(gstate.players[0].craft_pending
		&& gstate.players[0].craft_reply_serial == 0);

	reply.payload.recipe_craft.ticket = host_ticket;
	reply.payload.recipe_craft.player = 0;
	reply.payload.recipe_craft.reason = RECIPE_CRAFT_OK;
	clin_process(&reply);
	assert(!gstate.players[0].craft_pending
		&& gstate.players[0].craft_reason == RECIPE_CRAFT_OK);

	/* An out-of-range player index is dropped, not indexed. */
	reply.payload.recipe_craft.player = CLIENT_LOCAL_PLAYERS;
	clin_process(&reply);

	gstate.players[0].windows[WINDOWC_CRAFTING] = NULL;
	gstate.players[1].windows[WINDOWC_CRAFTING] = NULL;
	windowc_destroy(&host_wc);
	windowc_destroy(&guest_wc);
	memset(&gstate.players[0], 0, sizeof(gstate.players[0]));
	memset(&gstate.players[1], 0, sizeof(gstate.players[1]));
	puts("  client: a craft reply is installed on the player it names, only");
}

static void client_refuses_unusable_windows(uint16_t recipe) {
	unsigned attempts = 0;
	struct client_session session = {0};
	session.visible_window = -1;

	/* No window at all. */
	assert(!client_session_recipe_craft_request(&session, 0, WINDOWC_CRAFTING,
		recipe, refuse_request, &attempts));

	struct window_container chest;
	assert(windowc_create(&chest, WINDOW_TYPE_CHEST, CHEST_SIZE));
	session.windows[WINDOWC_CHEST] = &chest;
	session.visible_window = WINDOWC_CHEST;
	/* A chest is not a crafting window. */
	assert(!client_session_recipe_craft_request(&session, 0, WINDOWC_CHEST,
		recipe, refuse_request, &attempts));

	struct window_container wc;
	assert(windowc_create(&wc, WINDOW_TYPE_WORKBENCH, CRAFTING_SIZE));
	session.windows[WINDOWC_CRAFTING] = &wc;
	/* Not the visible window. */
	assert(!client_session_recipe_craft_request(&session, 0, WINDOWC_CRAFTING,
		recipe, refuse_request, &attempts));
	session.visible_window = WINDOWC_CRAFTING;
	/* A grid the player already laid out by hand. */
	windowc_slot_change(&wc, CRAFTING_SLOT_INPUT,
		(struct item_data) {.id = BLOCK_LOG, .count = 1});
	assert(!client_session_recipe_craft_request(&session, 0, WINDOWC_CRAFTING,
		recipe, refuse_request, &attempts));
	windowc_slot_change(&wc, CRAFTING_SLOT_INPUT, (struct item_data) {0});
	/* A held cursor stack. */
	windowc_slot_change(&wc, SPECIAL_SLOT_PICKED_ITEM,
		(struct item_data) {.id = BLOCK_LOG, .count = 1});
	assert(!client_session_recipe_craft_request(&session, 0, WINDOWC_CRAFTING,
		recipe, refuse_request, &attempts));
	windowc_slot_change(&wc, SPECIAL_SLOT_PICKED_ITEM, (struct item_data) {0});
	/* An out-of-range local player index. */
	assert(!client_session_recipe_craft_request(&session, CLIENT_LOCAL_PLAYERS,
		WINDOWC_CRAFTING, recipe, refuse_request, &attempts));

	/* None of the above ever reached the transport. */
	assert(attempts == 0);
	/* And a transport that refuses the send leaves nothing pending. */
	assert(!client_session_recipe_craft_request(&session, 0, WINDOWC_CRAFTING,
		recipe, refuse_request, &attempts));
	assert(attempts == 1 && !session.craft_pending);

	session.windows[WINDOWC_CRAFTING] = NULL;
	session.windows[WINDOWC_CHEST] = NULL;
	windowc_destroy(&wc);
	windowc_destroy(&chest);
	puts("  client: unusable windows are refused before any RPC is sent");
}

/* ---------------------------------------------------------------- server -- */

static struct server_local server;
static struct server_player guest;

struct replies {
	unsigned completions, slot_updates;
	enum recipe_craft_reason last_reason;
	uint32_t last_ticket;
	uint8_t last_player, last_window;
};
static void capture_reply(const struct client_rpc* call, void* user) {
	struct replies* out = user;
	if(call->type == CRPC_INVENTORY_SLOT) {
		out->slot_updates++;
		return;
	}
	if(call->type != CRPC_RECIPE_CRAFT) return;
	out->completions++;
	out->last_reason = call->payload.recipe_craft.reason;
	out->last_ticket = call->payload.recipe_craft.ticket;
	out->last_player = call->payload.recipe_craft.player;
	out->last_window = call->payload.recipe_craft.window;
}

static unsigned total_of(const struct inventory* inv, uint16_t id) {
	unsigned n = 0;
	for(size_t k = 0; k < inv->capacity; k++)
		if(inv->items[k].id == id) n += inv->items[k].count;
	return n;
}

static void bring_up_actor(struct server_player* player, struct replies* out) {
	assert(inventory_create(&player->inventory, &inventory_logic_player,
		&server, player, INVENTORY_SIZE, 0, 0, 0));
	player->inventory.reply = capture_reply;
	player->inventory.reply_user = out;
	player->active_inventory = &player->inventory;
	player->has_pos = true;
	player->finished_loading = true;
	player->health = 20;
}

static struct server_rpc craft_rpc(uint32_t ticket, uint16_t recipe,
	uint8_t player, uint8_t window) {
	return (struct server_rpc) {.type = SRPC_RECIPE_CRAFT,
		.payload.recipe_craft = {.ticket = ticket, .recipe_index = recipe,
			.player = player, .window = window}};
}

static void server_applies_each_ticket_at_most_once(uint16_t recipe) {
	struct replies host = {0};
	memset(&server, 0, sizeof(server));
	bring_up_actor(&server.player, &host);
	server.player.inventory.items[INVENTORY_SLOT_HOTBAR]
		= (struct item_data) {.id = BLOCK_LOG, .count = 3};

	struct server_rpc rpc = craft_rpc(10, recipe, 0, WINDOWC_INVENTORY);
	server_local_test_process_rpc(&server, &rpc);
	assert(host.completions == 1 && host.last_reason == RECIPE_CRAFT_OK);
	assert(host.last_ticket == 10 && host.last_player == 0
		&& host.last_window == WINDOWC_INVENTORY);
	assert(host.slot_updates > 0); /* slot state precedes the reply */
	assert(total_of(&server.player.inventory, BLOCK_LOG) == 2);
	assert(total_of(&server.player.inventory, BLOCK_PLANKS) == 4);

	struct item_data after[INVENTORY_SIZE];
	memcpy(after, server.player.inventory.items, sizeof(after));

	/* The exact retry replays the verdict and applies NOTHING. */
	unsigned slots_before = host.slot_updates;
	server_local_test_process_rpc(&server, &rpc);
	assert(host.completions == 2 && host.last_reason == RECIPE_CRAFT_OK);
	assert(host.slot_updates == slots_before);
	assert(!memcmp(after, server.player.inventory.items, sizeof(after)));

	/* An older ticket is stale, and named as such. */
	struct server_rpc old = craft_rpc(9, recipe, 0, WINDOWC_INVENTORY);
	server_local_test_process_rpc(&server, &old);
	assert(host.last_ticket == 9
		&& host.last_reason == RECIPE_CRAFT_STALE_TICKET);
	assert(!memcmp(after, server.player.inventory.items, sizeof(after)));

	/* Ticket zero is malformed. */
	struct server_rpc zero = craft_rpc(0, recipe, 0, WINDOWC_INVENTORY);
	server_local_test_process_rpc(&server, &zero);
	assert(host.last_reason == RECIPE_CRAFT_INVALID_REQUEST);

	/* A forged owning player is refused... */
	struct server_rpc wrong_player = craft_rpc(11, recipe, 1, WINDOWC_INVENTORY);
	server_local_test_process_rpc(&server, &wrong_player);
	assert(host.last_reason == RECIPE_CRAFT_WRONG_PLAYER);
	/* ...as is a window this actor does not have open... */
	struct server_rpc wrong_window = craft_rpc(12, recipe, 0, WINDOWC_CRAFTING);
	server_local_test_process_rpc(&server, &wrong_window);
	assert(host.last_reason == RECIPE_CRAFT_WRONG_WINDOW);
	/* ...and a still-loading actor is busy, not silently dropped. */
	server.player.finished_loading = false;
	struct server_rpc loading = craft_rpc(13, recipe, 0, WINDOWC_INVENTORY);
	server_local_test_process_rpc(&server, &loading);
	assert(host.last_reason == RECIPE_CRAFT_BUSY && host.last_ticket == 13);
	server.player.finished_loading = true;
	/* Sleeping likewise: answered BUSY rather than dropped, so the client's
	 * one in-flight request always terminates. */
	server.player.sleeping = true;
	struct server_rpc asleep = craft_rpc(14, recipe, 0, WINDOWC_INVENTORY);
	server_local_test_process_rpc(&server, &asleep);
	assert(host.last_reason == RECIPE_CRAFT_BUSY && host.last_ticket == 14);
	server.player.sleeping = false;

	assert(!memcmp(after, server.player.inventory.items, sizeof(after)));
	/* Every single request produced exactly one reply. */
	assert(host.completions == 8);
	inventory_destroy(&server.player.inventory, &server.player);
	puts("  server: one verdict per ticket, retries replay it, rejections name "
		 "a reason");
}

/* Untagged server->client sends, observed at the transport seam. The only one
 * this test can produce is the dropped-envelope craft answer. */
struct dropped {
	unsigned count;
	struct client_rpc last;
};
static void capture_untagged(const struct client_rpc* call, void* user) {
	struct dropped* out = user;
	if(call->type != CRPC_RECIPE_CRAFT) return;
	out->count++;
	out->last = *call;
}

/* Split screen, through the real dispatcher: the guest's own envelope may craft
 * from the guest's window, and may NOT claim to be the host. */
static void guest_envelope_owns_its_own_window(uint16_t recipe) {
	struct replies host = {0}, guest_replies = {0};
	memset(&server, 0, sizeof(server));
	memset(&guest, 0, sizeof(guest));
	bring_up_actor(&server.player, &host);
	bring_up_actor(&guest, &guest_replies);
	server.guest_actor = &guest;
	server.guest_generation = 7;
	server.guest_confirmed = true;
	server.player.inventory.items[INVENTORY_SLOT_HOTBAR]
		= (struct item_data) {.id = BLOCK_LOG, .count = 5};
	guest.inventory.items[INVENTORY_SLOT_HOTBAR]
		= (struct item_data) {.id = BLOCK_LOG, .count = 5};

	assert(svin_init());
	struct server_rpc_source guest_source = {.slot = 1, .generation = 7};
	struct server_rpc_source host_source = {0};

	/* The guest claiming to be player 0 is refused, and the HOST inventory is
	 * never touched -- the reply goes back to the guest that asked. */
	struct server_rpc forged = craft_rpc(100, recipe, 0, WINDOWC_INVENTORY);
	assert(svin_rpc_send_from(guest_source, &forged));
	server_local_test_process_messages(&server);
	assert(guest_replies.completions == 1
		&& guest_replies.last_reason == RECIPE_CRAFT_WRONG_PLAYER);
	assert(host.completions == 0);
	assert(total_of(&server.player.inventory, BLOCK_LOG) == 5);
	assert(total_of(&guest.inventory, BLOCK_LOG) == 5);

	/* Correctly owned, it crafts from the GUEST's inventory only. */
	struct server_rpc owned = craft_rpc(101, recipe, 1, WINDOWC_INVENTORY);
	assert(svin_rpc_send_from(guest_source, &owned));
	server_local_test_process_messages(&server);
	assert(guest_replies.completions == 2
		&& guest_replies.last_reason == RECIPE_CRAFT_OK);
	assert(total_of(&guest.inventory, BLOCK_LOG) == 4);
	assert(total_of(&guest.inventory, BLOCK_PLANKS) == 4);
	assert(total_of(&server.player.inventory, BLOCK_LOG) == 5);
	assert(total_of(&server.player.inventory, BLOCK_PLANKS) == 0);
	assert(host.completions == 0);

	/* The two actors keep independent ticket records: the host may reuse a
	 * number the guest already retired. */
	struct server_rpc host_rpc = craft_rpc(101, recipe, 0, WINDOWC_INVENTORY);
	assert(svin_rpc_send_from(host_source, &host_rpc));
	server_local_test_process_messages(&server);
	assert(host.completions == 1 && host.last_reason == RECIPE_CRAFT_OK);
	assert(total_of(&server.player.inventory, BLOCK_LOG) == 4);
	assert(guest_replies.completions == 2);

	/* A stale guest generation is not this guest at all: no actor is resolved,
	 * so nothing is inspected and nothing is mutated -- but the request is STILL
	 * ANSWERED (hostile review, finding 3). The client holds exactly one pending
	 * request and only a matching reply retires it; a silent drop is the one way
	 * the guide can wait forever. With no actor there is no inventory reply
	 * route, so the answer goes out on the untagged client queue -- observed
	 * here at the transport seam. */
	struct dropped drop = {0};
	clin_test_set_rpc_observer(capture_untagged, &drop);
	struct server_rpc_source stale = {.slot = 1, .generation = 6};
	struct server_rpc late = craft_rpc(102, recipe, 1, WINDOWC_INVENTORY);
	assert(svin_rpc_send_from(stale, &late));
	server_local_test_process_messages(&server);
	clin_test_set_rpc_observer(NULL, NULL);
	assert(guest_replies.completions == 2); /* not routed through an actor */
	assert(host.completions == 1);			/* and certainly not to the host */
	assert(drop.count == 1);
	assert(drop.last.payload.recipe_craft.reason == RECIPE_CRAFT_STALE_SESSION);
	/* Echoed verbatim, so the requesting session can recognise its own. */
	assert(drop.last.payload.recipe_craft.ticket == 102);
	assert(drop.last.payload.recipe_craft.player == 1);
	assert(drop.last.payload.recipe_craft.window == WINDOWC_INVENTORY);
	assert(total_of(&guest.inventory, BLOCK_LOG) == 4);

	inventory_destroy(&guest.inventory, &guest);
	inventory_destroy(&server.player.inventory, &server.player);
	server.guest_actor = NULL;
	puts("  server: the queue envelope owns the request, not the payload");
}

int main(void) {
	blocks_init();
	items_init();
	recipe_init();
	clin_init();
	uint16_t recipe = log_to_planks();
	client_retires_only_its_own_reply(recipe);
	client_retires_a_request_that_cannot_be_answered(recipe);
	reply_reaches_the_player_it_names(recipe);
	client_refuses_unusable_windows(recipe);
	server_applies_each_ticket_at_most_once(recipe);
	guest_envelope_owns_its_own_window(recipe);
	puts("PASS test_recipe_craft_ticket: at-most-once per actor, stale replies "
		 "inert, ownership from the envelope");
	return 0;
}
