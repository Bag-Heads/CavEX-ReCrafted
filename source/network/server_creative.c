/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include "server_creative.h"
#include "server_local_state.h" // full server state (split from server_local.h)

#include <assert.h>

#include "../item/inventory.h"
#include "../item/items.h"
#include "client_interface.h"

/* First empty slot in hotbar-then-main order, or SIZE_MAX when full. */
static size_t creative_find_empty_slot(struct inventory* inv) {
	for(size_t i = 0; i < INVENTORY_SIZE_HOTBAR; i++) {
		struct item_data cur;
		size_t slot = INVENTORY_SLOT_HOTBAR + i;
		if(!inventory_get_slot(inv, slot, &cur) || !item_get(&cur))
			return slot;
	}
	for(size_t i = 0; i < INVENTORY_SIZE_MAIN; i++) {
		struct item_data cur;
		size_t slot = INVENTORY_SLOT_MAIN + i;
		if(!inventory_get_slot(inv, slot, &cur) || !item_get(&cur))
			return slot;
	}
	return SIZE_MAX;
}

void server_creative_handle_give(struct server_local* s,
								 const struct server_rpc* call) {
	assert(s);
	server_creative_handle_give_for(s, &s->player, call);
}

void server_creative_handle_give_for(struct server_local* s,
	struct server_player* actor, const struct server_rpc* call) {
	assert(s && actor && call && call->type == SRPC_CREATIVE_GIVE);

	/* Server-side gate: a survival server ignores the RPC no matter what a
	 * client claims (same doctrine as the phase-1 damage gate). */
	if(!s->creative_mode)
		return;

	/* Registry validation: unregistered ids grant nothing. count is
	 * server-chosen -- the client never controls stack size. */
	struct item_data give = {
		.id = call->payload.creative_give.id,
		.durability = call->payload.creative_give.durability,
		.count = 1,
	};
	struct item* it = item_get(&give);
	if(!it)
		return;
	give.count = it->max_stack;

	size_t slot = creative_find_empty_slot(&actor->inventory);
	if(slot == SIZE_MAX)
		return; /* inventory full: predictable no-op */

	inventory_set_slot(&actor->inventory, slot, give);
	/* The inventory hook tags guest replies; the host keeps its old route. */
	server_local_window_reply(&actor->inventory, &(struct client_rpc) {
		.type = CRPC_INVENTORY_SLOT,
		.payload.inventory_slot.window = WINDOWC_INVENTORY,
		.payload.inventory_slot.slot = slot,
		.payload.inventory_slot.item = give,
	});
}
