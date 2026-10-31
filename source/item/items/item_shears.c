/*
	Shears block-break behaviour (Beta 1.7.3 ItemShears + BlockLeaves.harvestBlock).

	Unlike the place/use verbs, shears change what a block DROPS when broken: leaves broken while
	holding shears drop the leaf block itself (metadata & 3 == wood type) instead of the normal
	sapling roll. This helper is called from the dig path (server_local.c SRPC_BLOCK_DIG), which is
	where the held tool is known; it is forward-declared there to avoid a header touch.

	Deferred (documented): the faster shears dig speed (ItemShears.getStrVsBlock 15/leaves) is a
	client dig-speed feel concern, and shears durability (Beta maxDamage 238) waits on the absent
	tool-durability system -- same deferral as hoe/flint & steel. Cobweb/string + wool are not
	special-cased here (wool drops itself regardless of tool; web is not survival-obtainable).

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
