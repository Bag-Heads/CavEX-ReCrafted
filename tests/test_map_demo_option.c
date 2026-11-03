/* GPL-3.0-or-later. RetroMCP b1.7.3 ItemMap.onCreated:225-234:
 * unique map id, floor(player X/Z), scale 3, player's dimension. */
#include "support/split_residency_fixture.h"
#include "config.h"
#include "item/items.h"
#include "item/items/item_map.h"
#include "network/inventory_logic.h"
#include "network/map_data.h"
#include <stdio.h>

static void rpc(const struct client_rpc* call, void* unused) {
	(void)call; (void)unused;
	clin_drain_discard();
}
static void tick(struct server_local* s) {
	server_local_test_update_once(s);
	clin_drain_discard();
	/* Fixture consumes the load snapshot rather than running the GUI. */
	s->input_ack_pending = false;
}
static void run(const char* json, bool enabled) {
	struct config c = {.root = json_parse_string(json)};
	assert(c.root);
	struct server_local* s = split_fixture();
	string_set_str(s->level_name, split_path);
	s->map_demo = strcmp(config_read_string(&c, "debug.map_demo", "off"), "on") == 0
		|| config_read_int(&c, "debug.map_demo", 0) == 1;
	assert(s->map_demo == enabled);
	s->inventory_finish_pending = true;
	s->player.x = 10.75; s->player.z = -3.25;
	s->player.dimension = WORLD_DIM_OVERWORLD;
	for(unsigned i = 0; !s->player.finished_loading && i < 1000; i++) tick(s);
	assert(s->player.finished_loading);
	assert((s->map_demo_defer > 0) == enabled);
	inventory_set_hotbar(&s->player.inventory, 5);
	struct item_data original = {.id = ITEM_STICK, .count = 7};
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR + 5, original);
	struct item_data before[INVENTORY_SIZE];
	memcpy(before, s->player.inventory.items, sizeof(before));
	struct item_data prior = {.id = ITEM_MAP, .count = 1};
	assert(item_map.onCreated(s, &s->player, &prior));
	/* Freeze physical position, retaining complete production deferred ticks. */
	s->player.x = 10.75; s->player.z = -3.25;
	for(unsigned i = 0; i < 101; i++) {
		s->player.x = 10.75; s->player.z = -3.25;
		tick(s);
	}
	assert(inventory_get_hotbar(&s->player.inventory) == 5);
	if(enabled) {
		struct item_data held;
		assert(inventory_get_hotbar_item(&s->player.inventory, &held));
		assert(held.id == ITEM_MAP && held.count == 1);
		assert(held.durability != prior.durability);
		struct map_data* md = item_map_cache_resident(s, (int16_t)held.durability);
		assert(md && md->x_center == 10 && md->z_center == -4);
		assert(md->scale == 3 && md->dimension == WORLD_DIM_OVERWORLD);
		struct item_data crafted = {.id = ITEM_MAP, .count = 1};
		assert(item_map.onCreated(s, &s->player, &crafted));
		assert(crafted.durability != held.durability);
		before[INVENTORY_SLOT_HOTBAR + 5] = held;
	}
	assert(memcmp(before, s->player.inventory.items, sizeof(before)) == 0);
	/* A portal preserves the carried inventory and must not re-arm the demo. */
	s->player.finished_loading = false;
	s->inventory_finish_pending = true;
	s->preserve_live_inventory_on_finish = true;
	s->map_demo_defer = 0;
	for(unsigned i = 0; !s->player.finished_loading && i < 1000; i++) tick(s);
	assert(s->player.finished_loading && s->map_demo_defer == 0);
	assert(memcmp(before, s->player.inventory.items, sizeof(before)) == 0);
	split_destroy(s);
	config_destroy(&c);
}
int main(void) {
	(void)split_populate; /* shared fixture also offers optional population */
	assert(clin_init());
	blocks_init(); items_init();
	clin_test_set_rpc_observer(rpc, NULL);
	run("{}", false);
	run("{\"debug\":{\"map_demo\":\"off\"}}", false);
	run("{\"debug\":{\"map_demo\":0}}", false);
	run("{\"debug\":{\"map_demo\":\"on\"}}", true);
	run("{\"debug\":{\"map_demo\":1}}", true);
	clin_test_set_rpc_observer(NULL, NULL);
	puts("PASS map_demo off/on/1, selected slot, fresh craft and portal preservation");
	return 0;
}
