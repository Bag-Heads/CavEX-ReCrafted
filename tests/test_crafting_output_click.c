/*
	Audit fix [C4]: right-click on a crafting OUTPUT slot must behave exactly
	like a left-click on the whole stack.

	Beta 1.7.3 Container.slotClick runs the output-slot path for both var2==0
	(left) and var2==1 (right), and InventoryCraftResult.decrStackSize IGNORES
	the requested count -> a right-click yields the WHOLE result (one craft),
	never a half-split. This test pins that behaviour for BOTH the player
	2x2 crafting grid (inventory_player.c) and the 3x3 workbench
	(inventory_crafting.c).

	A multi-count recipe (1 log -> 4 planks) is used so a whole-stack pickup
	(count == 4) is distinguishable from an erroneous half-split (count == 2).

	Tests are forced -UNDEBUG by the CMake test rule, so the asserts below are
	always live.
*/

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "../source/item/inventory.h"
#include "../source/item/items.h"
#include "../source/item/recipe.h"
#include "../source/network/inventory_logic.h"
#include "../source/game/achievements/game_event.h"

/* --- minimal item registry stub -------------------------------------------

   The inventory click path only ever reads item_get(...)->max_stack (and, on
   the armour branch we never reach, ->armor.is_armor). We avoid pulling in the
   whole block/render/server registry (items.c) by providing a focused stub
   that gives the two items this test uses a vanilla 64 stack limit. */

static struct item stub_log;
static struct item stub_planks;

struct item* items[ITEMS_MAX];

struct item* item_get(struct item_data* item) {
	assert(item);
	if(item->id == 0)
		return NULL;
	if(item->id == BLOCK_LOG)
		return &stub_log;
	if(item->id == BLOCK_PLANKS)
		return &stub_planks;
	/* every other id used here is irrelevant to the click logic */
	return &stub_planks;
}

bool item_is_block(struct item_data* item) {
	(void)item;
	return true;
}

/* --- server_local stubs ---------------------------------------------------

   Referenced by the inventory_logic structs' on_create/on_close/on_destroy
   callbacks, which this test never triggers (it installs the logic AFTER
   inventory_create and never closes/destroys through the logic). They only
   need to resolve at link time. */

struct entity;
struct server_local;
/* This focused legacy fixture has no admitted achievement owner. ACH-1's
 * pooled test exercises the real resolver/emitter; these remain link doubles. */
uint64_t server_local_achievement_epoch(struct server_local* s,
	const struct server_player* player) { (void)s; (void)player; return 0; }
struct actor_ident server_local_actor_ident(const struct server_local* s,
	const struct server_player* player) { (void)s; (void)player; return ACTOR_IDENT_HOST; }
void game_event_emit(const struct game_event* ev) {
	(void)ev;
	assert(0 && "no admitted achievement owner in this focused fixture");
}
struct death_drop_provenance;
struct entity* server_local_spawn_item(vec3 pos, struct item_data* it,
									   bool throw, void* s) {
	(void)pos;
	(void)it;
	(void)throw;
	(void)s;
	assert(0 && "server_local_spawn_item must not be reached");
	return NULL;
}

bool server_local_spawn_item_batch(vec3 pos, const struct item_data* items,
									 size_t count, bool throw,
									 struct server_local* s) {
	(void)pos;
	(void)items;
	(void)count;
	(void)throw;
	(void)s;
	assert(0 && "server_local_spawn_item_batch must not be reached");
	return false;
}

bool server_local_spawn_sourced_item_batch_for(
	vec3 pos, const struct item_data* items,
	const struct death_drop_provenance* provenance, size_t count, bool throw,
	bool container_rng, struct server_local* s, struct server_player* player) {
	(void)player;
	(void)provenance;
	(void)container_rng;
	return server_local_spawn_item_batch(pos, items, count, throw, s);
}

void server_local_send_inv_changes(set_inv_slot_t changes, void* inv,
								   uint8_t window) {
	(void)changes;
	(void)inv;
	(void)window;
	assert(0 && "server_local_send_inv_changes must not be reached");
}

/* --- helpers --------------------------------------------------------------- */

static struct item_data make(uint16_t id, uint8_t count) {
	return (struct item_data) {.id = id, .durability = 0, .count = count};
}

/* Seed the crafting grid input slot with one log via the real engine flow
   (place picked log into the empty input slot) so post_action populates the
   output with 4 planks, exactly as in-game. */
/* pid seam: the dispatcher requires a non-NULL actor whenever a logic
 * is installed. The crafting ACTION callbacks never dereference it,
 * and the complete server_player type would drag the whole server
 * header set into this deliberately stub-based harness -- an opaque
 * non-NULL stand-in is the honest minimum. */
static unsigned char g_actor_storage;
#define g_actor ((struct server_player*)(void*)&g_actor_storage)

static void seed_one_log(struct inventory* inv, size_t input_slot) {
	set_inv_slot_t changes;
	set_inv_slot_init(changes);

	inventory_set_picked_item(inv, make(BLOCK_LOG, 1));
	bool ok = inventory_action(inv, g_actor, input_slot, false, changes);
	assert(ok);

	set_inv_slot_clear(changes);

	/* picked item consumed into the slot, output now holds the result */
	assert(!inventory_get_picked_item(inv, NULL));

	struct item_data in;
	assert(inventory_get_slot(inv, input_slot, &in));
	assert(in.id == BLOCK_LOG && in.count == 1);
}

static void assert_output_is_planks4(struct inventory* inv, size_t out_slot) {
	struct item_data out;
	assert(inventory_get_slot(inv, out_slot, &out));
	assert(out.id == BLOCK_PLANKS);
	assert(out.count == 4);
}

/* Run the right-click-with-empty-cursor scenario and return the picked stack
   that ended up on the cursor. Asserts the structural invariants common to
   both the whole-stack expectation checks. */
static struct item_data
do_output_click(struct inventory_logic* logic, size_t out_slot,
				size_t input_slot, bool right) {
	struct inventory inv;
	bool created = inventory_create(&inv, NULL, NULL, NULL, /*capacity*/ 64, 0, 0, 0);
	assert(created);
	inv.logic = logic; /* install after create -> on_create not invoked */

	seed_one_log(&inv, input_slot);
	assert_output_is_planks4(&inv, out_slot);

	set_inv_slot_t changes;
	set_inv_slot_init(changes);
	inventory_action(&inv, g_actor, out_slot, right, changes);
	set_inv_slot_clear(changes);

	struct item_data picked;
	bool have_picked = inventory_get_picked_item(&inv, &picked);
	assert(have_picked);

	/* input decremented by exactly one (1 log -> 0 -> empty slot) */
	assert(!inventory_get_slot(&inv, input_slot, NULL));
	/* output cleared (no second craft possible, input now empty) */
	assert(!inventory_get_slot(&inv, out_slot, NULL));

	free(inv.items);
	return picked;
}

static void test_grid(const char* label, struct inventory_logic* logic,
					  size_t out_slot, size_t input_slot) {
	(void)label;

	/* RIGHT-click, empty cursor -> WHOLE stack (4), not a half-split (2). */
	struct item_data r = do_output_click(logic, out_slot, input_slot, true);
	assert(r.id == BLOCK_PLANKS);
	assert(r.count == 4);

	/* LEFT-click, empty cursor -> also the whole stack. */
	struct item_data l = do_output_click(logic, out_slot, input_slot, false);
	assert(l.id == BLOCK_PLANKS);
	assert(l.count == 4);

	/* right-click result == left-click result */
	assert(r.id == l.id && r.count == l.count && r.durability == l.durability);

	/* RIGHT-click onto a matching stackable cursor -> merges the WHOLE result
	   (cursor 4 + result 4 = 8), inputs decremented by one, output cleared. */
	{
		struct inventory inv;
		bool created
			= inventory_create(&inv, NULL, NULL, NULL, 64, 0, 0, 0);
		assert(created);
		inv.logic = logic;

		seed_one_log(&inv, input_slot);
		assert_output_is_planks4(&inv, out_slot);

		inventory_set_picked_item(&inv, make(BLOCK_PLANKS, 4));

		set_inv_slot_t changes;
		set_inv_slot_init(changes);
		inventory_action(&inv, g_actor, out_slot, true, changes);
		set_inv_slot_clear(changes);

		struct item_data picked;
		assert(inventory_get_picked_item(&inv, &picked));
		assert(picked.id == BLOCK_PLANKS);
		assert(picked.count == 8); /* whole result merged, not a single unit */

		assert(!inventory_get_slot(&inv, input_slot, NULL));
		assert(!inventory_get_slot(&inv, out_slot, NULL));

		free(inv.items);
	}
}

int main(void) {
	/* set up vanilla stack limits for the two items under test */
	memset(&stub_log, 0, sizeof(stub_log));
	memset(&stub_planks, 0, sizeof(stub_planks));
	stub_log.max_stack = 64;
	stub_planks.max_stack = 64;
	for(int k = 0; k < ITEMS_MAX; k++)
		items[k] = NULL;

	recipe_init();

	/* player 2x2 grid */
	test_grid("player", &inventory_logic_player, INVENTORY_SLOT_OUTPUT,
			  INVENTORY_SLOT_CRAFTING);
	/* 3x3 workbench */
	test_grid("crafting", &inventory_logic_crafting, CRAFTING_SLOT_OUTPUT,
			  CRAFTING_SLOT_INPUT);

	return 0;
}
