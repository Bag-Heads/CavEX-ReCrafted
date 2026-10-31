/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdint.h>

#include "../block/blocks.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_world.h"
#include "../util.h"
#include "java_random.h"
#include "populate.h"
#include "tree.h"
#include "tree_runtime.h"

// wgen_world get() over the live server world. Out-of-world / not-yet-loaded
// columns read as air, matching both the wgen_world contract (get MUST return
// BLOCK_AIR out of bounds) and Beta's World.getBlockId, so the builders' air/leaf
// checks behave identically at a chunk edge.
static uint8_t rt_get(void* ctx, int x, int y, int z) {
	struct server_local* s = ctx;
	struct block_data b;
	if(!server_world_get_block(&s->world, x, y, z, &b))
		return BLOCK_AIR;
	return b.type;
}

// wgen_world set() over the live server world. server_world_set_block notifies
// neighbours (Beta's growTree used the no-notify setBlock/setBlockAndMetadata);
// for logs/leaves/dirt the only effect of the notify is lighting + client sync,
// never a block-changing cascade, so the resulting world is identical.
static void rt_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	struct server_local* s = ctx;
	server_world_set_block(
		&s->worlds[0], x, y, z, (struct block_data) {.type = id, .metadata = meta});
}

bool server_grow_tree(struct server_local* s, int x, int y, int z,
					  int sapling_meta) {
	struct wgen_world w = {
		.ctx = s,
		.get = rt_get,
		.set = rt_set,
		.height = NULL,
		.skylight = NULL,
		.set_chest = NULL,
	};

	// Seed a fresh java_random from the runtime entropy source. Two xorshift draws
	// give a 48-bit-worth seed; everything the tree needs (the big-tree nextInt(10)
	// roll + the builder's own draws) then comes from this java_random, mirroring
	// Beta's single world.rand stream. See tree_runtime.h on why this is geometry-
	// faithful but intentionally not a byte-exact Beta draw stream.
	struct java_random jr;
	uint64_t seed = ((uint64_t)rand_gen(&s->rand_src) << 32)
		^ (uint64_t)rand_gen(&s->rand_src);
	jrand_set_seed(&jr, (int64_t)seed);

	return worldgen_grow_sapling(&w, &jr, sapling_meta, x, y, z);
}
