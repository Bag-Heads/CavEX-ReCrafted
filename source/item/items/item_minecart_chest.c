/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Beta Item.minecartCrate = new ItemMinecart(86, 1) [740c583: Item.java
 * static initializer] -- shiftedIndex 342, minecartType 1. Mirrors
 * item_minecart.c (the rideable cart's item) exactly, differing only in the
 * spawned minecart_type (ItemMinecart.onItemUse passes its own
 * this.minecartType straight through to the EntityMinecart coordinate
 * constructor -- [740c583: ItemMinecart.onItemUse]). */

#include <assert.h>

#include "../../network/server_local.h"
#include "../../network/client_interface.h"
#include "../../network/server_interface.h"
#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side)
{
	// Same rail acceptance as the rideable cart (BlockRail.isRailBlock: rail
	// OR powered rail OR detector rail). [740c583: ItemMinecart.onItemUse;
	// BlockRail.isRailBlock]
	if (!on
		|| (on->block->type != BLOCK_RAIL
			&& on->block->type != BLOCK_POWERED_RAIL
			&& on->block->type != BLOCK_DETECTOR_RAIL))
		return false;

	vec3 pos;
	item_minecart_spawn_position(on, pos);

	return server_local_spawn_minecart_type_w(pos,
		server_local_world_of(s, actor), 1) != NULL;
}

struct item item_minecart_chest = {
	.name         = "Minecart with Chest",
	.has_damage   = false,
	.max_stack    = 1, // vehicles don't stack in Beta 1.7.3
	.fuel         = 0,
	.renderItem   = render_item_flat,
	.onItemPlace  = onItemPlace,
	.armor.is_armor = false,
	.tool.type      = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			// Beta Item.minecartCrate.setIconCoord(7, 9). [740c583: Item.java]
			.texture_x = 7,
			.texture_y = 9,
		},
	},
};
