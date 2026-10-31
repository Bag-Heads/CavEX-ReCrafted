/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
#include "../item/inventory_click.h"
#include "inventory_logic.h"
#include "window_registry.h"

/* The type comes from the server registry; the same transfer implementation
 * runs in the client window. Expected-stack comparison belongs AFTER mutation
 * in NetServerHandler, never in this operation. [740c583:498-507] */
struct item_data inventory_quick_move(struct inventory* inv, struct server_player* player,
	size_t slot, set_inv_slot_t changes) {
	const struct window_desc* desc = inv ? window_registry_find(inv->logic) : NULL;
	if(!player || !desc || !changes || !inventory_slot_valid(inv, slot) || inv->invalidated)
		return (struct item_data){0};
	struct item_data result = inventory_transfer(inv, desc->wire_type, slot, changes, inv->user);
	if(!set_inv_slot_empty_p(changes) && inv->logic->post_action)
		inv->logic->post_action(inv, player, slot, false, true, changes);
	return result;
}
