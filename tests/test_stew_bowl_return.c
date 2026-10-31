/* Beta ItemSoup.onItemRightClick returns a new empty-bowl stack. */
#include "support/split_residency_fixture.h"
#include "network/server_local_guest.h"
#include "item/items.h"
#include <stdio.h>

static enum client_rpc_type host_rpcs[8];
static unsigned host_rpc_count;
static void observe_host(const struct client_rpc* call, void* user) {
	(void)user;
	assert(host_rpc_count < 8);
	host_rpcs[host_rpc_count++] = call->type;
}

static unsigned item_entities(struct server_local* s) {
	unsigned count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM) count++;
		dict_entity_next(it);
	}
	return count;
}

static void eat(struct server_local* s, struct server_player* actor,
				struct server_rpc_source source) {
	actor->health = MAX_PLAYER_HEALTH / 2;
	actor->has_pos = actor->finished_loading = true;
	inventory_set_hotbar(&actor->inventory, 0);
	size_t slot = INVENTORY_SLOT_HOTBAR;
	inventory_set_slot(&actor->inventory, slot,
				   (struct item_data){.id = ITEM_MUSHROOM_STEW, .count = 1});
	short health = actor->health;
	unsigned drops = item_entities(s);
	struct server_rpc use = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = 0, .y = 3, .z = 0, .side = SIDE_TOP}};
	if(source.slot) {
		assert(svin_rpc_send_from(source, &use));
		server_local_test_process_messages(s);
	} else {
		host_rpc_count = 0;
		clin_test_set_rpc_observer(observe_host, NULL);
		server_local_test_process_rpc(s, &use);
		clin_test_set_rpc_observer(NULL, NULL);
		assert(host_rpc_count == 2);
		assert(host_rpcs[0] == CRPC_PLAYER_SET_HEALTH);
		assert(host_rpcs[1] == CRPC_INVENTORY_SLOT);
	}
	struct item_data held = actor->inventory.items[slot];
	assert(actor->health == health + 5 * HEALTH_PER_HEART);
	assert(held.id == ITEM_BOWL && held.count == 1 && held.durability == 0);
	assert(item_entities(s) == drops);
}

int main(void) {
	assert(clin_init());
	assert(svin_init());
	blocks_init();
	items_init();
	struct server_local* s = split_fixture();
	(void)split_populate(s, 0, 0);
	s->player.active_inventory = &s->player.inventory;
	s->player.finished_loading = true;
	struct block_data ground;
	struct server_world* world = server_local_world_of(s, &s->player);
	assert(world);
	assert(server_world_get_block(world, 0, 3, 0, &ground));
	assert(ground.type == BLOCK_GRASS);
	assert(server_world_get_block(world, 0, 4, 0, &ground));
	assert(ground.type == BLOCK_AIR);
	eat(s, &s->player, (struct server_rpc_source){0});
	puts("PASS host stew returns bowl to held slot without item entity");
	struct server_rpc_source guest_source;
	assert(server_local_guest_create(s, &guest_source));
	s->guest_confirmed = s->guest_gameplay = true;
	svin_set_split_accept(true);
	struct server_player* guest = s->guest_actor;
	guest->x = s->player.x;
	guest->y = s->player.y;
	guest->z = s->player.z;
	eat(s, guest, guest_source);
	assert(s->player.inventory.items[INVENTORY_SLOT_HOTBAR].id == ITEM_BOWL);
	puts("PASS guest stew returns bowl to guest held slot without item entity");
	inventory_destroy(&guest->inventory, guest);
	free(guest);
	s->guest_actor = NULL;
	split_destroy(s);
	return 0;
}
