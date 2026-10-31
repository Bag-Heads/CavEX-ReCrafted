/*
	Beta 1.7.3 CAKE item (Item.cake = new ItemReed(98, Block.cake), id 354,
	maxStackSize 1). Placing it emits BLOCK_CAKE (92) at metadata 0 through
	ItemReed.onItemUse -> World.canBlockBePlacedAt -> BlockCake.canPlaceBlockAt,
	whose canBlockStay requires a SOLID material under the target cell (a cake
	can never float). One item is consumed per successful place (the dispatch's
	inventory_consume; max_stack 1 == Beta --stackSize emptying the slot).
	Eating (blockActivated -> eatCakeSlice) already lives in block_cake.c.
	[740c583: Item.java cake = ItemReed(98, Block.cake); ItemReed.onItemUse;
	 BlockCake.canPlaceBlockAt/canBlockStay]

	Moved out of items_object.h (the item_dye precedent) to carry the engine
	headers the placement verb needs.
*/

#include "../../block/blocks.h"
#include "../../block/fluid_block.h"
#include "../../graphics/render_item.h"
#include "../../network/server_local.h"
#include "../../network/server_local_state.h" // full server state (split from server_local.h)
#include "../../network/server_world.h"

static bool onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
						struct block_info* where, struct block_info* on,
						enum side on_side) {
	(void)it;
	if(!s || !where)
		return false;

	// BlockCake.canBlockStay: getBlockMaterial(x, y-1, z).isSolid(). Reuse the
	// engine's canonical Material.isSolid() mapping (fluid_block.h); an
	// out-of-world read refuses, matching Beta's air-below refusal.
	struct block_data below;
	if(!server_world_get_block(block_world(s), where->x, where->y - 1, where->z,
							   &below)
	   || below.type == BLOCK_AIR
	   || !fluid_block_material_is_solid_id(below.type))
		return false;

	// Fresh cake is metadata 0 (no bites); block_place_default supplies the
	// replaceability + entity-obstruction gates and the SET_BLOCK lifecycle.
	struct item_data cake_block
		= {.id = BLOCK_CAKE, .durability = 0, .count = 1};
	return block_place_default(s, actor, &cake_block, where, on, on_side);
}

struct item item_cake = {
	.name = "Cake",
	.has_damage = false,
	.max_stack = 1, // Beta ItemReed(98, cake).setMaxStackSize(1)
	.fuel = 0,
	.renderItem = render_item_flat,
	.onItemPlace = onItemPlace,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 13,
			.texture_y = 1,
		},
	},
};
