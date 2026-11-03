/* ci-1101/15: test the identity the CLIENT actually holds, not the new
 * server-side damage read back into a test-local stack. Beta ItemMap.onUpdate
 * mutates the same ItemStack that ItemRenderer renders in single player. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "game/game_state.h"
#include "item/items.h"
#include "item/items/item_map.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_creative.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"

static struct server_local s;
static unsigned planes, slots;
static int32_t plane_id;
static void observe(const struct client_rpc* call, void* unused) {
	(void)unused;
	if(call->type == CRPC_MAP_DATA) {
		planes++;
		plane_id = call->payload.map_data.slot->map_id;
	}
	if(call->type == CRPC_INVENTORY_SLOT) slots++;
}
static unsigned nonzero(const uint8_t* colors) {
	unsigned n=0;
	for(unsigned i=0;i<MAP_DATA_COLORS_SIZE;i++) n += colors[i] != 0;
	return n;
}
static bool run(bool picker) {
	inventory_clear(&s.player.inventory);
	struct inventory* client = windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]);
	inventory_clear(client);
	struct item_data stack={.id=ITEM_MAP,.count=1};
	if(picker) {
		s.creative_mode = true;
		server_creative_handle_give(&s, &(struct server_rpc){
			.type=SRPC_CREATIVE_GIVE, .payload.creative_give.id=ITEM_MAP});
	} else {
		/* Real survival creation hook; full recipe RPC has its own fixture. */
		s.creative_mode = false;
		assert(items[ITEM_MAP]->onCreated(&s,&s.player,&stack));
		inventory_set_slot(&s.player.inventory,INVENTORY_SLOT_HOTBAR,stack);
		clin_rpc_send(&(struct client_rpc){.type=CRPC_INVENTORY_SLOT,
			.payload.inventory_slot={.window=WINDOWC_INVENTORY,
				.slot=INVENTORY_SLOT_HOTBAR,.item=stack}});
	}
	clin_update_drain();
	assert(inventory_get_slot(client,INVENTORY_SLOT_HOTBAR,&stack));
	unsigned before=stack.durability;
	planes=slots=0;
	server_local_test_tick_item_updates(&s);
	clin_update_drain();
	struct item_data server_stack;
	assert(inventory_get_slot(&s.player.inventory,INVENTORY_SLOT_HOTBAR,&server_stack));
	assert(inventory_get_slot(client,INVENTORY_SLOT_HOTBAR,&stack));
	struct map_data* md=item_map_cache_resident(&s,(int16_t)server_stack.durability);
	assert(md);
	const uint8_t* colors=clin_held_map_colors((int16_t)stack.durability,NULL);
	bool equal=!memcmp(colors,md->colors,MAP_DATA_COLORS_SIZE);
	printf("%s: initial=%u server=%u client=%u plane=%d planes=%u slot_updates=%u server_pixels=%u client_pixels=%u equal=%d\n",
		picker?"picker":"crafted",before,server_stack.durability,stack.durability,
		plane_id,planes,slots,nonzero(md->colors),nonzero(colors),equal);
	bool ok=stack.durability==server_stack.durability && planes==1
		&& nonzero(colors)>0 && equal;
	/* Progressive scanning may send more pixels; an unchanged identity
	 * must not keep rebasing the client inventory every tick. */
	slots=0;
	server_local_test_tick_item_updates(&s);
	clin_update_drain();
	printf("%s: next tick identity slot_updates=%u\n",picker?"picker":"crafted",slots);
	return ok && slots==0;
}
int main(void) {
	char dir[128]; test_fixture_dir("test_map_mirror",dir,sizeof(dir));
#ifdef _WIN32
	assert(_mkdir(dir)==0);
#else
	assert(mkdir(dir,0777)==0);
#endif
	assert(clin_init()); blocks_init(); items_init();
	memset(&gstate,0,sizeof(gstate)); world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	struct window_container wc;
	assert(windowc_create(&wc,WINDOW_TYPE_INVENTORY,INVENTORY_SIZE));
	gstate.players[0].windows[WINDOWC_INVENTORY]=&wc;
	string_init_set_str(s.level_name,dir);
	for(unsigned i=0;i<MAX_MAP_CACHE;i++) s.map_cache_id[i]=INT32_MIN;
	server_world_create(&s.worlds[0],s.level_name,WORLD_DIM_OVERWORLD,4242,true);
	s.worlds[0].session=&s;
	/* Production world handles are live only after a generation is assigned. */
	s.worlds[0].generation=1;
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory,&inventory_logic_player,&s,&s.player,INVENTORY_SIZE,0,0,0));
	s.player.active_inventory=&s.player.inventory;
	s.player.has_pos=s.player.finished_loading=true;
	s.player.health=MAX_PLAYER_HEALTH;
	s.player.y=64;
	struct server_chunk* sc;
	assert(server_world_load_chunk(&s.worlds[0],0,0,&sc));
	clin_test_set_rpc_observer(observe,NULL);
	/* Picker FIRST: damage zero is absent before its fallback allocates. */
	bool picker=run(true), crafted=run(false);
	clin_test_set_rpc_observer(NULL,NULL);
	fflush(stdout);
	test_fixture_dir_cleanup(dir);
	if(!picker || !crafted) return 1;
	puts("PASS picker + crafted: one tick, client-held ID and pixels equal server");
	return 0;
}
