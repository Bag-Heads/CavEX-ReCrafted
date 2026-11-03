/*
	Shears block-break behaviour (Beta 1.7.3 ItemShears + BlockLeaves.harvestBlock).

	Unlike the place/use verbs, shears change what a block DROPS when broken: leaves broken while
	holding shears drop the leaf block itself (metadata & 3 == wood type) instead of the normal
	sapling roll. This helper is called from the dig path (server_local.c SRPC_BLOCK_DIG), which is
	where the held tool is known; it is forward-declared there to avoid a header touch.

	Dig speed (ItemShears.java:22-24 getStrVsBlock 15/web+leaves, 5/wool) lives in
	tool.c (tool_dig_delay_env_ms); wear (ItemShears.java:10-13, 1 per leaves/web) lives in
	items.c item_wear_on_block_break. Wool drops itself regardless of tool.

	Web harvest (ci-1101/44): Block.web's material (Material.java:30 field_31068_A) is
	setNoHarvest(), so InventoryPlayer.canHarvestBlock (InventoryPlayer.java:270-277) asks the
	held stack; ItemShears.canHarvestBlock (ItemShears.java:18-20) says yes for web only.
	item_shears_can_harvest() is that predicate, for the dig drop gate in server_local.c.

	Decompile: 740c583:1.7.3-LTS ItemShears, BlockLeaves.harvestBlock.
*/

#include <stdbool.h>
#include <stdint.h>

#include "../../block/blocks.h"
#include "../../item/items.h"

// If `held` is shears and `blk` has a special sheared drop, fill *out with it and return true.
// Currently: LEAVES -> the leaf block (metadata & 3 strips the decay/check bits to the wood type).
bool item_shears_block_drop(struct block_data blk, struct item_data held,
							struct item_data* out) {
	if(!out || held.id != ITEM_SHEARS)
		return false;

	if(blk.type == BLOCK_LEAVES) {
		*out = (struct item_data) {
			.id = BLOCK_LEAVES,
			.durability = (uint16_t)(blk.metadata & 0x3),
			.count = 1,
		};
		return true;
	}

	return false;
}

// Beta ItemShears.canHarvestBlock (mcb173-retromcp ItemShears.java:18-20):
//   return var1.blockID == Block.web.blockID;
// True when `held_id` is shears and `block_type` is web (BLOCK_COBWEB == Block.web, id 30).
bool item_shears_can_harvest(uint8_t block_type, uint16_t held_id) {
	return held_id == ITEM_SHEARS && block_type == BLOCK_COBWEB;
}
