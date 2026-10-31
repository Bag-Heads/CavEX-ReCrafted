#include <assert.h>
#include <string.h>

#include "block/blocks.h"
#include "network/server_world.h"

struct block* blocks[256];

static void test_tick(struct server_local* s, struct block_info* info) {
	(void)s;
	(void)info;
}

static void drain_tick(struct server_world* world, uint32_t* fires) {
	size_t entries = world->scheduled_tick_count;
	for(size_t i = 0; i < entries; i++) {
		struct scheduled_tick tick;
		assert(server_world_scheduled_tick_pop(world, &tick));
		if(tick.fire_at_tick <= world->current_tick) {
			(*fires)++;
			assert(tick.block_type == BLOCK_REDSTONE_TORCH_LIT);
			assert(server_world_schedule_tick_raw(world, tick.x, tick.y, tick.z,
											  tick.block_type, 1));
		} else {
			server_world_scheduled_tick_requeue(world, tick);
		}
	}
}

int main(void) {
	struct block torch;
	memset(&torch, 0, sizeof(torch));
	torch.onWorldTick = test_tick;
	torch.tick_delay = 1;
	blocks[BLOCK_REDSTONE_TORCH_LIT] = &torch;

	struct server_world world;
	memset(&world, 0, sizeof(world));
	server_world_scheduled_ticks_create(
		&world, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);

	for(int i = 0; i < 10; i++) {
		assert(server_world_schedule_block_tick_raw(
			&world, i, 32, 0, BLOCK_REDSTONE_TORCH_LIT));
	}
	assert(world.scheduled_tick_count == 10);

	uint32_t fires = 0;
	for(uint32_t tick = 1; tick <= 100; tick++) {
		world.current_tick = tick;
		drain_tick(&world, &fires);
		assert(world.scheduled_tick_count == 10);
	}
	assert(fires == 1000);
	assert(world.scheduled_tick_drops == 0);

	for(int i = 0; i < 1000; i++)
		assert(!server_world_schedule_block_tick_raw(
			&world, i, 10, 1, BLOCK_STONE));
	assert(world.scheduled_tick_count == 10);

	server_world_scheduled_ticks_destroy(&world);
	return 0;
}
