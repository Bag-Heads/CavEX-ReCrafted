/* Copyright (c) 2026 CaveWii contributors. GPL-3.0-or-later. */
#include "inventory_achievements.h"
#include "server_local.h"
#include "server_local_guest.h"
#include "../game/achievements/game_event.h"

static void emit_item(struct inventory* inv, uint8_t kind,
	struct item_data result) {
	/* ACH-1 MP choice (b): only the local provider's owned inventories.
	 * Prediction mirrors and guided scratch inventories cannot award. */
	if(!inv || !inv->logic || !inv->user || !inv->owner || !result.count)
		return;
	struct server_local* s = inv->user;
	uint64_t epoch = server_local_achievement_epoch(s, inv->owner);
	if(!epoch) return;
	struct actor_ident who = server_local_actor_ident(s, inv->owner);
	who.generation = epoch; /* host transport generation is zero */
	game_event_emit(&(struct game_event){.kind = kind,
		.authority = GE_AUTH_SERVER, .who = who,
		.u.item = {.id = result.id, .count = result.count}});
}

void inventory_achievement_craft_taken(struct inventory* inv, size_t slot,
	struct item_data result) {
	/* Beta RetroMCP SlotCrafting.java:17-35: only these eight ids award;
	 * ContainerPlayer.java:91 uses the same SlotCrafting for the 2x2 grid. */
	if(slot != 0) return;
	switch(result.id) {
		case 58: case 270: case 61: case 290:
		case 297: case 354: case 274: case 268:
			emit_item(inv, GE_CRAFT_TAKEN, result);
			break;
		default: break;
	}
}

void inventory_achievement_furnace_taken(struct inventory* inv, size_t slot,
	struct item_data result) {
	/* Beta RetroMCP SlotFurnace.java:15-25, ContainerFurnace.java:96. */
	if(slot == FURNACE_SLOT_OUTPUT && (result.id == 265 || result.id == 350))
		emit_item(inv, GE_FURNACE_TAKEN, result);
}

void inventory_achievement_collected(struct inventory* inv,
	struct server_player* player, struct item_data before) {
	/* Beta RetroMCP EntityItem.java:110-119: server-side only, AFTER
	 * addItemStackToInventory returns true. Each handler captures before
	 * inventory_collect drains the stack; a failed/partial take never calls us. */
	if(inv && player && inv->owner == player
	   && (before.id == 17 || before.id == 334))
		emit_item(inv, GE_ITEM_PICKED_UP, before);
}
