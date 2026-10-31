/*
	Beta 1.7.3 MILK BUCKET (Item.bucketMilk = new ItemBucket(79, -1), id 335).

	b1.7.3 milk is NOT drinkable: milk is an ItemBucket, not an ItemFood, and no
	status effects exist to clear. Its ONLY right-click behavior is the ItemBucket
	isFull < 0 branch: if the 5.0D ray trace hits a TILE within reach (and
	canMineBlock passes -- always true in survival SP), the held stack becomes an
	EMPTY bucket. Nothing is placed, no sound plays, the clicked block is
	untouched. Obtained exclusively via EntityCow.interact (whole-slot replace of
	a held empty bucket -- see the SRPC_ENTITY_USE dispatch in server_local.c).
	[740c583: ItemBucket.onItemRightClick isFull<0 branch; Item.java:353]

	setContainerItem(bucketEmpty): crafting with a milk bucket (the cake recipe)
	leaves an empty bucket IN THE CRAFTING GRID (Beta SlotCrafting semantics) --
	see container_item in items.h and the crafting consume loops.
*/

#include "../../graphics/render_item.h"
#include "../../network/server_local.h"

struct item item_bucket_milk = {
	.name = "Milk",
	.has_damage = false,
	.max_damage = 0,
	.max_stack = 1, // Beta ItemBucket ctor setMaxStackSize(1)
	.fuel = 0,
	.renderItem = render_item_flat,
	/* No onUseOnBlock: milk is ItemBucket(-1); item_bucket_use_no_target
	 * pours it on any ray tile hit [ItemBucket.java:51-53]. */
	.container_item = ITEM_BUCKET, // Beta Item.java:353 setContainerItem(bucketEmpty)
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 13, // Beta bucket row in gui/items.png: 10 empty, 11
			.texture_y = 4,  // water, 12 lava, 13 milk
		},
	},
};
