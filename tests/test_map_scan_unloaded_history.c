/* GPL-3.0-or-later. RetroMCP b1.7.3 ItemMap.func_28011_a:40-211:
 * var15 = var31 follows real terrain sampling, before the circular write gate.
 * Missing terrain must not invent a predecessor height for the next read. */
#include <assert.h>
#include <stdio.h>
#include "block/blocks.h"
#include "item/items/item_map.h"
#include "network/map_data.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "support/test_fixture_dir.h"

static struct server_local s;
static struct map_data md;
static void grass(int z) {
	server_world_set_block(&s.worlds[0], 0, 64, z,
		(struct block_data){.type = BLOCK_GRASS});
	uint8_t height;
	assert(server_world_get_height(&s.worlds[0], 0, z, &height) && height == 65);
}
static uint8_t scan(void) {
	map_data_set_defaults(&md, 0, 0, WORLD_DIM_OVERWORLD);
	md.scale = 0;
	md.step = 15; /* next stripe contains px=64 (world x=0) */
	md.colors[64 + 64 * MAP_DATA_DIM] = 37; /* missing world z=0 */
	item_map_update_data_w(&s.worlds[0], &md, 0, 0);
	return md.colors[64 + 80 * MAP_DATA_DIM]; /* first loaded z=16 */
}
int main(void) {
	char path[256];
	test_fixture_dir("map_scan_unloaded_history", path, sizeof(path));
	blocks_init();
	string_init_set_str(s.level_name, path);
	server_world_create(&s.worlds[0], s.level_name, WORLD_DIM_OVERWORLD, 42, true);
	s.worlds[0].session = &s;
	struct server_chunk* chunk;
	assert(server_world_load_chunk(&s.worlds[0], 0, -1, &chunk));
	assert(server_world_load_chunk(&s.worlds[0], 0, 1, &chunk));
	grass(-1); grass(16);
	uint8_t height;
	assert(!server_world_get_height(&s.worlds[0], 0, 0, &height));
	uint8_t skipped = scan();
	assert(md.colors[64 + 64 * MAP_DATA_DIM] == 37);
	/* Skip z=0..15 entirely: previous read is height 65 at z=-1.
	 * Target height 65 => (65-65)*4/5 - .2 = -.2 => grass*4+1=5. */
	printf("unloaded-gap target=%u; previous-loaded-height oracle=5\n", (unsigned)skipped);
	fflush(stdout);
	assert(skipped == 5);
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &chunk));
	for(int z = 0; z < 16; z++) grass(z);
	assert(scan() == skipped); /* equal-height resident predecessors */
	server_world_discard(&s.worlds[0]);
	string_clear(s.level_name);
	test_fixture_dir_cleanup(path);
	puts("PASS unloaded samples preserve map height history");
	return 0;
}
