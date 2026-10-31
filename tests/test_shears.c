/*
	Golden test for Beta 1.7.3 shears block-break drops (ItemShears + BlockLeaves.harvestBlock):
	leaves broken WHILE HOLDING SHEARS drop the leaf block itself (metadata & 3 == wood type),
	not the normal sapling. Pure unit test over the item_shears_block_drop helper -- no world.

	Decompile: 740c583:1.7.3-LTS ItemShears, BlockLeaves.harvestBlock.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "item/items.h"

extern bool item_shears_block_drop(struct block_data blk, struct item_data held,
								   struct item_data* out);

static void test_leaves_with_shears_drop_leaf(void) {
	struct item_data shears = {.id = ITEM_SHEARS, .count = 1};
	struct item_data out = {0};
	// birch leaves (type 2), no decay/check bits set
	struct block_data leaves = {.type = BLOCK_LEAVES, .metadata = 2};
	assert(item_shears_block_drop(leaves, shears, &out) == true);
	assert(out.id == BLOCK_LEAVES && out.durability == 2 && out.count == 1);
	printf("  leaves(type 2) + shears -> leaf block, durability 2, count 1 OK\n");
}

static void test_leaf_metadata_masked_to_type(void) {
	struct item_data shears = {.id = ITEM_SHEARS, .count = 1};
	struct item_data out = {0};
	// spruce leaves (type 1) with the decay bit 0x4 and check bit 0x8 set -> drop type only (& 3)
	struct block_data leaves = {.type = BLOCK_LEAVES, .metadata = 0x8 | 0x4 | 1};
	assert(item_shears_block_drop(leaves, shears, &out) == true);
	assert(out.durability == 1); // & 3 strips the decay/check bits to the wood type
	printf("  leaves(meta 0xD) + shears -> leaf block type 1 (decay/check bits stripped) OK\n");
}

static void test_leaves_without_shears_noop(void) {
	struct item_data hand = {.id = 0, .count = 0};	  // empty hand
	struct item_data pick = {.id = 257, .count = 1};  // iron pickaxe, not shears
	struct item_data out = {0};
	struct block_data leaves = {.type = BLOCK_LEAVES, .metadata = 0};
	assert(item_shears_block_drop(leaves, hand, &out) == false);
	assert(item_shears_block_drop(leaves, pick, &out) == false);
	printf("  leaves without shears (hand / pickaxe): no special drop OK\n");
}

static void test_shears_on_non_leaf_noop(void) {
	struct item_data shears = {.id = ITEM_SHEARS, .count = 1};
	struct item_data out = {0};
	struct block_data stone = {.type = BLOCK_STONE, .metadata = 0};
	assert(item_shears_block_drop(stone, shears, &out) == false);
	printf("  shears on stone: no special drop OK\n");
}

int main(void) {
	printf("test_shears:\n");
	test_leaves_with_shears_drop_leaf();
	test_leaf_metadata_masked_to_type();
	test_leaves_without_shears_noop();
	test_shears_on_non_leaf_noop();
	printf("PASS test_shears\n");
	return 0;
}
