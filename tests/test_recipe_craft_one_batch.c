/*
	Hostile-review finding 1 (lane/craft-rpc): "one batch" must be an ENFORCED
	invariant, not a lucky property of today's recipe registry.

	inventory_transfer's craft branch is Beta's own recursion
	[740c583 Container.java:67-77]: after SlotCrafting.onPickupFromSlot consumes
	the grid it re-runs the matcher and loops again while the newly produced
	output stack is SMALLER than the one just taken. That is exactly right for a
	player holding shift on the output slot -- and exactly wrong for the guided
	"craft one batch of recipe R" transaction, which priced ONE batch in its
	capacity preflight and promised the client one batch in its reply.

	The loop cannot fire from the guided entry today, for two reasons that are
	both properties of the current REGISTRY rather than of the code:

	  * inventory_crafting.c's recipe_craft_take pins every grid cell to count 1,
	    so the only thing craft_consume can leave behind is a container item; and
	  * ITEM_BUCKET is the only container_item Beta 1.7.3 defines
	    [740c583 Item.java:344/345/353 setContainerItem(bucketEmpty), mirrored by
	    source/item/items/item_bucket_{water,lava,milk}.c], and no registered
	    recipe is made of empty buckets alone.

	Register ONE more container_item whose returned containers happen to form a
	registered pattern and the guided path would quietly craft twice -- spending
	containers the player was owed and overrunning a preflight that only ever
	priced one batch. So this test builds precisely that registry, proves the
	repeat is real by driving the shared manual primitives directly, and then
	proves the transaction refuses it with zero mutation.

	Nothing here touches Beta semantics: the synthetic items/recipes live only in
	this process, and the real bucket recipes are re-checked as a positive
	control so the new gate cannot be a false alarm on shipped content.
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../source/block/blocks.h"
#include "../source/item/inventory_click.h"
#include "../source/item/items.h"
#include "../source/item/recipe.h"
#include "../source/network/inventory_crafting.h"

/* Two independent synthetic families, high in the id space (ITEMS_MAX is 2304;
 * Beta's own highest ids are the 2256/2257 records). Family 1 makes the
 * container grid produce a SMALLER result, which is what actually re-enters the
 * loop; family 2 makes it produce an equally large one, which instead strands a
 * second output in slot 0. Both are "not one batch" and both must be refused. */
#define SYN1_INGREDIENT 2300
#define SYN1_CONTAINER 2301
#define SYN1_RESULT_A 2302 /* 4 per batch, from the ingredient */
#define SYN1_RESULT_B 2303 /* 1 per batch, from the returned containers */

#define SYN2_INGREDIENT 2296
#define SYN2_CONTAINER 2297
#define SYN2_RESULT_A 2298 /* 4 per batch */
#define SYN2_RESULT_B 2299 /* also 4 per batch */

static struct item syn1_ingredient = {.max_stack = 1,
	.container_item = SYN1_CONTAINER};
static struct item syn1_container = {.max_stack = 64};
static struct item syn1_result_a = {.max_stack = 64};
static struct item syn1_result_b = {.max_stack = 64};
static struct item syn2_ingredient = {.max_stack = 1,
	.container_item = SYN2_CONTAINER};
static struct item syn2_container = {.max_stack = 64};
static struct item syn2_result_a = {.max_stack = 64};
static struct item syn2_result_b = {.max_stack = 64};

static void install(uint16_t id, struct item* def) {
	assert(id < ITEMS_MAX);
	assert(!items[id]); /* never shadow a real registration */
	items[id] = def;
}

/* Index of the recipe registered last. */
static uint16_t last_shaped(void) {
	return (uint16_t)(array_recipe_size(recipes_crafting) - 1);
}

static uint16_t syn1_batch, syn2_batch;

static void register_synthetic_registry(void) {
	install(SYN1_INGREDIENT, &syn1_ingredient);
	install(SYN1_CONTAINER, &syn1_container);
	install(SYN1_RESULT_A, &syn1_result_a);
	install(SYN1_RESULT_B, &syn1_result_b);
	install(SYN2_INGREDIENT, &syn2_ingredient);
	install(SYN2_CONTAINER, &syn2_container);
	install(SYN2_RESULT_A, &syn2_result_a);
	install(SYN2_RESULT_B, &syn2_result_b);

	/* 2x2 of the ingredient -> 4 of result A. */
	recipe_add(recipes_crafting,
		(struct item_data) {.id = SYN1_RESULT_A, .count = 4}, 2, 2,
		(uint8_t[]) {1, 1, 1, 1},
		(struct item_data) {.id = SYN1_INGREDIENT}, false);
	syn1_batch = last_shaped();
	/* 2x2 of the CONTAINER the recipe above hands back -> 1 of result B. One
	 * is smaller than four, so Beta's shift-click recursion would run again. */
	recipe_add(recipes_crafting,
		(struct item_data) {.id = SYN1_RESULT_B, .count = 1}, 2, 2,
		(uint8_t[]) {1, 1, 1, 1},
		(struct item_data) {.id = SYN1_CONTAINER}, false);

	recipe_add(recipes_crafting,
		(struct item_data) {.id = SYN2_RESULT_A, .count = 4}, 2, 2,
		(uint8_t[]) {1, 1, 1, 1},
		(struct item_data) {.id = SYN2_INGREDIENT}, false);
	syn2_batch = last_shaped();
	/* Same size, so the recursion stops -- and leaves a SECOND output stack
	 * sitting in the crafting slot the transaction promised to hand back empty. */
	recipe_add(recipes_crafting,
		(struct item_data) {.id = SYN2_RESULT_B, .count = 4}, 2, 2,
		(uint8_t[]) {1, 1, 1, 1},
		(struct item_data) {.id = SYN2_CONTAINER}, false);
}

/* The 2x2 corner of a 3x3 workbench grid. */
static const size_t corner[4]
	= {CRAFTING_SLOT_INPUT + 0, CRAFTING_SLOT_INPUT + 1,
		CRAFTING_SLOT_INPUT + 3, CRAFTING_SLOT_INPUT + 4};

static void seed_four(struct inventory* inv, uint16_t ingredient) {
	for(size_t k = 0; k < 4; k++)
		inv->items[CRAFTING_SLOT_HOTBAR + k]
			= (struct item_data) {.id = ingredient, .count = 1};
}

static unsigned total_of(const struct inventory* inv, uint16_t id) {
	unsigned n = 0;
	for(size_t k = 0; k < inv->capacity; k++)
		if(inv->items[k].id == id) n += inv->items[k].count;
	return n;
}

/* Proof the fixture is real: driven through the SAME primitives the transaction
 * uses (the recipe matcher + Container.transferStackInSlot), an unguarded
 * one-batch layout genuinely crafts family 1 TWICE. */
static void the_repeat_is_real(void) {
	struct inventory inv;
	assert(inventory_create(&inv, NULL, NULL, NULL, CRAFTING_SIZE, 0, 0, 0));
	for(size_t k = 0; k < 4; k++)
		inv.items[corner[k]]
			= (struct item_data) {.id = SYN1_INGREDIENT, .count = 1};
	inventory_click_post(&inv, WINDOW_TYPE_WORKBENCH, 1, NULL);
	assert(inv.items[CRAFTING_SLOT_OUTPUT].id == SYN1_RESULT_A
		&& inv.items[CRAFTING_SLOT_OUTPUT].count == 4);
	(void)inventory_transfer(&inv, WINDOW_TYPE_WORKBENCH, CRAFTING_SLOT_OUTPUT,
		NULL, NULL);
	/* One shift-click, TWO batches: the four containers were consumed by the
	 * second one and result B was produced on top of result A. */
	assert(total_of(&inv, SYN1_RESULT_A) == 4);
	assert(total_of(&inv, SYN1_RESULT_B) == 1);
	assert(total_of(&inv, SYN1_CONTAINER) == 0);
	inventory_destroy(&inv, NULL);
	puts("  fixture verified: one unguarded shift-click really crafts twice");
}

/* Same layout through the guided transaction: refused, with nothing touched. */
static void transaction_refuses_the_repeat(uint16_t recipe, uint16_t ingredient,
	const char* label) {
	struct inventory inv;
	uint16_t maps = 99;
	assert(inventory_create(&inv, NULL, NULL, NULL, CRAFTING_SIZE, 0, 0, 0));
	inv.client_map_counter = &maps;
	seed_four(&inv, ingredient);
	struct item_data before[CRAFTING_SIZE];
	memcpy(before, inv.items, sizeof(before));

	set_inv_slot_t changes;
	set_inv_slot_init(changes);
	assert(inventory_crafting_one_batch(&inv, WINDOW_TYPE_WORKBENCH, recipe,
			   NULL, changes)
		== RECIPE_CRAFT_NOT_ONE_BATCH);
	/* Refusal means refusal: not one byte, not one reported slot, no map id. */
	assert(!memcmp(before, inv.items, sizeof(before)));
	assert(set_inv_slot_size(changes) == 0);
	assert(maps == 99);
	set_inv_slot_clear(changes);
	inventory_destroy(&inv, NULL);
	printf("  %s refused as not-one-batch, zero mutation\n", label);
}

/* Positive control: the real container_item content still crafts. The gate must
 * refuse a genuine second batch, never a legitimately returned bucket. */
static void real_bucket_recipes_still_craft(void) {
	uint16_t cake = RECIPE_CRAFT_INDEX_NONE;
	for(size_t n = 0; n < array_recipe_size(recipes_crafting); n++)
		if(array_recipe_cget(recipes_crafting, n)->result.id == ITEM_CAKE) {
			cake = (uint16_t)n;
			break;
		}
	assert(cake != RECIPE_CRAFT_INDEX_NONE);

	struct inventory inv;
	assert(inventory_create(&inv, NULL, NULL, NULL, CRAFTING_SIZE, 0, 0, 0));
	for(size_t k = 0; k < 3; k++)
		inv.items[CRAFTING_SLOT_HOTBAR + k]
			= (struct item_data) {.id = ITEM_MILK_BUCKET, .count = 1};
	inv.items[CRAFTING_SLOT_HOTBAR + 3]
		= (struct item_data) {.id = ITEM_SUGAR, .count = 8};
	inv.items[CRAFTING_SLOT_HOTBAR + 4]
		= (struct item_data) {.id = ITEM_EGG, .count = 4};
	inv.items[CRAFTING_SLOT_HOTBAR + 5]
		= (struct item_data) {.id = ITEM_WHEAT, .count = 8};
	assert(inventory_crafting_one_batch(&inv, WINDOW_TYPE_WORKBENCH, cake, NULL,
			   NULL)
		== RECIPE_CRAFT_OK);
	assert(total_of(&inv, ITEM_CAKE) == 1);
	assert(total_of(&inv, ITEM_BUCKET) == 3); /* the containers really came back */
	assert(total_of(&inv, ITEM_MILK_BUCKET) == 0);
	inventory_destroy(&inv, NULL);
	puts("  positive control: cake still crafts and returns all 3 buckets");
}

/* And the whole shipped registry, in both grids, still produces exactly one
 * batch -- the gate is proven not to reject a single real recipe. */
static void every_registered_recipe_still_passes(void) {
	unsigned ok = 0;
	size_t total = inventory_crafting_recipe_count();
	for(uint16_t index = 0; index < (uint16_t)total; index++) {
		const struct recipe* r = index < array_recipe_size(recipes_crafting)
			? array_recipe_cget(recipes_crafting, index) : NULL;
		if(r && (r->result.id == SYN1_RESULT_A || r->result.id == SYN1_RESULT_B
					|| r->result.id == SYN2_RESULT_A
					|| r->result.id == SYN2_RESULT_B))
			continue; /* the synthetic fixture, refused on purpose above */
		struct inventory inv;
		assert(inventory_create(&inv, NULL, NULL, NULL, CRAFTING_SIZE, 0, 0, 0));
		/* One free stack per grid cell the pattern needs, respecting max_stack
		 * so unstackable ingredients occupy separate slots. */
		size_t used = 0;
		if(r) {
			for(size_t k = 0; k < r->width * r->height; k++) {
				struct item_data want = r->shape[k].item;
				if(!want.id) continue;
				struct item* def = item_get(&want);
				unsigned cap = def && def->max_stack ? def->max_stack : 1;
				size_t s;
				for(s = 0; s < used; s++) {
					struct item_data* it = &inv.items[CRAFTING_SLOT_HOTBAR + s];
					if(it->id == want.id && it->durability == want.durability
					   && it->count < cap)
						break;
				}
				if(s == used) {
					assert(used < INVENTORY_SIZE_HOTBAR);
					inv.items[CRAFTING_SLOT_HOTBAR + used] = want;
					inv.items[CRAFTING_SLOT_HOTBAR + used].count = 1;
					used++;
				} else inv.items[CRAFTING_SLOT_HOTBAR + s].count++;
			}
		} else {
			const struct recipe_shapeless* sr
				= array_recipe_shapeless_cget(recipes_crafting_shapeless,
					index - array_recipe_size(recipes_crafting));
			for(size_t k = 0; k < sr->count; k++) {
				assert(used < INVENTORY_SIZE_HOTBAR);
				inv.items[CRAFTING_SLOT_HOTBAR + used] = sr->ingredients[k].item;
				inv.items[CRAFTING_SLOT_HOTBAR + used].count = 1;
				used++;
			}
		}
		enum recipe_craft_reason reason = inventory_crafting_one_batch(&inv,
			WINDOW_TYPE_WORKBENCH, index, NULL, NULL);
		assert(reason != RECIPE_CRAFT_NOT_ONE_BATCH);
		if(reason == RECIPE_CRAFT_OK) ok++;
		inventory_destroy(&inv, NULL);
	}
	assert(ok > 100);
	printf("  %u shipped recipes crafted; not one tripped the one-batch gate\n",
		ok);
}

int main(void) {
	blocks_init();
	items_init();
	recipe_init();
	real_bucket_recipes_still_craft();
	register_synthetic_registry();
	the_repeat_is_real();
	transaction_refuses_the_repeat(syn1_batch, SYN1_INGREDIENT,
		"a second, smaller batch off the returned containers");
	transaction_refuses_the_repeat(syn2_batch, SYN2_INGREDIENT,
		"a second output stranded in the crafting slot");
	every_registered_recipe_still_passes();
	puts("PASS test_recipe_craft_one_batch: the transaction delivers exactly one "
		 "batch or nothing at all");
	return 0;
}
