/*
	Requirement 2 (lane/craft-rpc): capacity preflight.

	The guided craft must compute the COMPLETE outcome -- the result stack AND
	every container item the grid would hand back -- and reject with ZERO
	mutation when it cannot all fit. The approved spec's words: "no hidden
	overflow, lost ingredients or false success."

	Beta already refuses a craft it cannot deliver: Container.transferStackInSlot
	returns null the moment mergeItemStack moves nothing, BEFORE
	SlotCrafting.onPickupFromSlot ever consumes an ingredient [740c583
	Container.java:192-248, SlotCrafting.java:36-43]. The runnable oracle spells
	the same gate out literally -- ../mc173 mc173-server/src/player.rs wraps its
	whole craft-result branch in `if main_inv.can_push(result_stack)`, and only
	then calls `craft_tracker.consume(...)`.

	The guided path owes ONE thing more than Beta's single shift-click: the
	containers Beta leaves sitting in the crafting grid (SlotCrafting puts the
	empty bucket back in the cell it came from) would be destroyed when the
	guided action finishes, since the player never opened that grid. So the
	returned containers count against capacity too. The cake recipe is the exact
	fixture for this: three MILK buckets in, one cake plus three EMPTY buckets
	out, and an empty bucket is unstackable (max_stack 1, Beta ItemBucket
	setMaxStackSize(1)) -- so the batch needs FOUR free slots, not one.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/block/blocks.h"
#include "../source/item/items.h"
#include "../source/item/recipe.h"
#include "../source/network/inventory_crafting.h"

static enum recipe_craft_reason checked_one_batch(struct inventory* inv,enum window_type type,
 uint16_t recipe,struct server_local* server,set_inv_slot_t changes) {
 struct inventory before;memcpy(&before,inv,sizeof before);
 struct item_data items[CRAFTING_SIZE];memcpy(items,inv->items,inv->capacity*sizeof(*items));
 uint16_t maps=inv->client_map_counter?*inv->client_map_counter:0;
 enum recipe_craft_reason verdict=inventory_crafting_one_batch_preflight(inv,type,recipe);
 assert(inventory_crafting_recipe_eligibility(inv,type,recipe)==verdict);
 inv->transaction_pending=true;inv->picked_item=(struct item_data){.id=BLOCK_DIRT,.count=1};
 inv->items[1]=(struct item_data){.id=BLOCK_DIRT,.count=1};
 assert(inventory_crafting_one_batch_preflight(inv,type,recipe)==RECIPE_CRAFT_BUSY);
 assert(inventory_crafting_recipe_eligibility(inv,type,recipe)==verdict);
 *inv=before;memcpy(inv->items,items,inv->capacity*sizeof(*items));
 assert(!memcmp(&before,inv,sizeof before)&&!memcmp(items,inv->items,inv->capacity*sizeof(*items)));
 assert(!inv->client_map_counter||*inv->client_map_counter==maps);
 enum recipe_craft_reason result=inventory_crafting_one_batch(inv,type,recipe,server,changes);
 assert(result==verdict);return result;
}

static uint16_t recipe_making(uint16_t result_id) {
	for(size_t n = 0; n < array_recipe_size(recipes_crafting); n++)
		if(array_recipe_cget(recipes_crafting, n)->result.id == result_id)
			return (uint16_t)n;
	assert(!"recipe result not registered");
	return RECIPE_CRAFT_INDEX_NONE;
}

/* Every player slot full of an unrelated, already-maxed stack: no room to
 * merge into and no empty slot, except where the fixture opens one. */
static void pack_full(struct inventory* inv) {
	for(size_t k = CRAFTING_SLOT_MAIN; k < CRAFTING_SIZE; k++)
		inv->items[k] = (struct item_data) {.id = BLOCK_COBBLESTONE, .count = 64};
}

/* Lay out the cake ingredients so that crafting frees EXACTLY `free_after`
 * slots: the three milk buckets always empty their (unstackable) slots, and
 * the egg stack empties too when `spend_egg_slot` is set. */
static void seed_cake(struct inventory* inv, bool spend_egg_slot) {
	pack_full(inv);
	inv->items[CRAFTING_SLOT_MAIN + 0]
		= (struct item_data) {.id = ITEM_MILK_BUCKET, .count = 1};
	inv->items[CRAFTING_SLOT_MAIN + 1]
		= (struct item_data) {.id = ITEM_MILK_BUCKET, .count = 1};
	inv->items[CRAFTING_SLOT_MAIN + 2]
		= (struct item_data) {.id = ITEM_MILK_BUCKET, .count = 1};
	/* Deliberately leftovers, so these slots stay occupied after the craft. */
	inv->items[CRAFTING_SLOT_MAIN + 3]
		= (struct item_data) {.id = ITEM_SUGAR, .count = 8};
	inv->items[CRAFTING_SLOT_MAIN + 4]
		= (struct item_data) {.id = ITEM_EGG, .count = spend_egg_slot ? 1 : 4};
	inv->items[CRAFTING_SLOT_MAIN + 5]
		= (struct item_data) {.id = ITEM_WHEAT, .count = 8};
}

static unsigned total_of(const struct inventory* inv, uint16_t id) {
	unsigned n = 0;
	for(size_t k = 0; k < inv->capacity; k++)
		if(inv->items[k].id == id) n += inv->items[k].count;
	return n;
}

/* Three free slots is one short: the cake fits, the third bucket does not. */
static void rejects_when_containers_do_not_fit(uint16_t cake) {
	struct inventory inv;
	uint16_t maps = 77;
	assert(inventory_create(&inv, NULL, NULL, NULL, CRAFTING_SIZE, 0, 0, 0));
	inv.client_map_counter = &maps;
	seed_cake(&inv, false);
	struct item_data before[CRAFTING_SIZE];
	memcpy(before, inv.items, sizeof(before));

	set_inv_slot_t changes;
	set_inv_slot_init(changes);
	assert(checked_one_batch(&inv, WINDOW_TYPE_WORKBENCH, cake, NULL,
			   changes)
		== RECIPE_CRAFT_NO_CAPACITY);
	/* Not one byte, and not one reported slot change. */
	assert(!memcmp(before, inv.items, sizeof(before)));
	assert(set_inv_slot_size(changes) == 0);
	assert(maps == 77);
	set_inv_slot_clear(changes);
	inventory_destroy(&inv, NULL);
	puts("  3 free slots: cake + 3 returned buckets rejected, zero mutation");
}

/* Freeing the fourth slot makes it an exact fit -- and the whole batch lands. */
static void exact_fit_succeeds_with_every_container_returned(uint16_t cake) {
	struct inventory inv;
	uint16_t maps = 77;
	assert(inventory_create(&inv, NULL, NULL, NULL, CRAFTING_SIZE, 0, 0, 0));
	inv.client_map_counter = &maps;
	seed_cake(&inv, true);

	set_inv_slot_t changes;
	set_inv_slot_init(changes);
	assert(checked_one_batch(&inv, WINDOW_TYPE_WORKBENCH, cake, NULL,
			   changes)
		== RECIPE_CRAFT_OK);
	assert(set_inv_slot_size(changes) > 0);
	assert(total_of(&inv, ITEM_CAKE) == 1);
	assert(total_of(&inv, ITEM_BUCKET) == 3);
	assert(total_of(&inv, ITEM_MILK_BUCKET) == 0);
	assert(total_of(&inv, ITEM_EGG) == 0);
	/* One of each remaining ingredient consumed, the rest untouched. */
	assert(total_of(&inv, ITEM_SUGAR) == 6);  /* 8 - 2 */
	assert(total_of(&inv, ITEM_WHEAT) == 5);  /* 8 - 3 */
	/* The grid is handed back empty: nothing was stranded in a crafting cell. */
	for(size_t k = CRAFTING_SLOT_OUTPUT; k < CRAFTING_SLOT_MAIN; k++)
		assert(!inv.items[k].id);
	/* And every slot the caller was told about really did change. */
	set_inv_slot_it_t it;
	unsigned reported = 0;
	for(set_inv_slot_it(it, changes); !set_inv_slot_end_p(it);
		set_inv_slot_next(it)) {
		assert(*set_inv_slot_cref(it) < CRAFTING_SIZE);
		reported++;
	}
	assert(reported == set_inv_slot_size(changes));
	set_inv_slot_clear(changes);
	inventory_destroy(&inv, NULL);
	puts("  4 free slots: exact fit crafts and returns all 3 buckets");
}

/* Missing ingredients are a rejection, not a partial craft. */
static void rejects_missing_ingredients(uint16_t cake) {
	struct inventory inv;
	assert(inventory_create(&inv, NULL, NULL, NULL, CRAFTING_SIZE, 0, 0, 0));
	seed_cake(&inv, true);
	inv.items[CRAFTING_SLOT_MAIN + 2] = (struct item_data) {0}; /* one milk short */
	struct item_data before[CRAFTING_SIZE];
	memcpy(before, inv.items, sizeof(before));
	assert(checked_one_batch(&inv, WINDOW_TYPE_WORKBENCH, cake, NULL,
			   NULL)
		== RECIPE_CRAFT_MISSING_INGREDIENTS);
	assert(!memcmp(before, inv.items, sizeof(before)));
	inventory_destroy(&inv, NULL);
	puts("  one ingredient short: rejected with the ingredients left alone");
}

/* A result that cannot merge anywhere is refused before anything is consumed --
 * the plain Beta gate, without any container-item subtlety. */
static void rejects_when_result_does_not_fit(void) {
	uint16_t planks = RECIPE_CRAFT_INDEX_NONE;
	for(size_t n = 0; n < array_recipe_size(recipes_crafting); n++) {
		const struct recipe* r = array_recipe_cget(recipes_crafting, n);
		if(r->width == 1 && r->height == 1 && r->shape[0].item.id == BLOCK_LOG
		   && r->result.id == BLOCK_PLANKS) {
			planks = (uint16_t)n;
			break;
		}
	}
	assert(planks != RECIPE_CRAFT_INDEX_NONE);

	struct inventory inv;
	assert(inventory_create(&inv, NULL, NULL, NULL, CRAFTING_SIZE, 0, 0, 0));
	pack_full(&inv);
	/* The only log stack is full, so consuming one leaves no empty slot and
	 * planks cannot merge into a full cobblestone stack either. */
	inv.items[CRAFTING_SLOT_HOTBAR] = (struct item_data) {.id = BLOCK_LOG, .count = 64};
	struct item_data before[CRAFTING_SIZE];
	memcpy(before, inv.items, sizeof(before));
	assert(checked_one_batch(&inv, WINDOW_TYPE_WORKBENCH, planks,
			   NULL, NULL)
		== RECIPE_CRAFT_NO_CAPACITY);
	assert(!memcmp(before, inv.items, sizeof(before)));

	/* Opening a single slot is enough for a result with no containers. */
	inv.items[CRAFTING_SLOT_MAIN] = (struct item_data) {0};
	assert(checked_one_batch(&inv, WINDOW_TYPE_WORKBENCH, planks,
			   NULL, NULL)
		== RECIPE_CRAFT_OK);
	assert(total_of(&inv, BLOCK_PLANKS) == 4);
	assert(total_of(&inv, BLOCK_LOG) == 63);
	inventory_destroy(&inv, NULL);
	puts("  full inventory: result-only recipe refused, then fits in one slot");
}

int main(void) {
	blocks_init();
	items_init();
	recipe_init();
	uint16_t cake = recipe_making(ITEM_CAKE);
	rejects_when_containers_do_not_fit(cake);
	exact_fit_succeeds_with_every_container_returned(cake);
	rejects_missing_ingredients(cake);
	rejects_when_result_does_not_fit();
	puts("PASS test_recipe_craft_capacity: preflight covers result AND returned "
		 "containers, rejection mutates nothing");
	return 0;
}
