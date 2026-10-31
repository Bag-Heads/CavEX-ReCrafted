#include <assert.h>
#include <string.h>

#include "block/blocks.h"
#include "network/server_world.h"
#include "profile.h"

struct block* blocks[256];

int main(void) {
	struct server_world world;
	memset(&world, 0, sizeof(world));
	server_world_scheduled_ticks_create(
		&world, SERVER_WORLD_SCHEDULED_TICK_INITIAL_CAPACITY);

	/* The entity admission cap is per logical update and cannot be consumed by
	 * the independent block/fluid queue. Rejected callers retain their own source
	 * (primed TNT/fully-fused creeper) and retry after the next reset. */
	server_world_begin_update(&world);
	for(int i = 0; i < FCAVEX_MAX_EXPLOSIONS_PER_TICK; i++)
		assert(server_world_try_begin_explosion(&world));
	assert(!server_world_try_begin_explosion(&world));

	for(size_t i = 0; i < world.scheduled_tick_capacity; i++)
		assert(server_world_schedule_tick_raw(
			&world, (w_coord_t)i, 64, 0, BLOCK_WATER_FLOW, 10000));
	assert(world.scheduled_tick_count == world.scheduled_tick_capacity);
	assert(!server_world_try_begin_explosion(&world));

	server_world_begin_update(&world);
	assert(server_world_try_begin_explosion(&world));
	assert(world.scheduled_tick_count == world.scheduled_tick_capacity);
	server_world_scheduled_ticks_destroy(&world);
	return 0;
}
