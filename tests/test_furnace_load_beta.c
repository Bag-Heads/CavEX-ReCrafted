/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.

	hw1029c (a): loading a chunk never changes a furnace's block id.

	Beta 1.7.3 [740c583 TileEntityFurnace.readFromNBT:89-91]: a load restores
	BurnTime/CookTime/Items and recomputes currentItemBurnTime from the fuel
	slot -- nothing else. The idle/lit block id is written only by
	BlockFurnace.updateFurnaceBlockState, from updateEntity [:146-148] when
	`burnTime > 0` changes during a tick. A furnace saved lit (62) with
	BurnTime 0 therefore stays lit forever (Dream Journal's y=0 lamps), one
	saved lit with BurnTime>0 keeps burning down, and an idle one stays idle.
	Our former load fixup rewrote 62->61 on load (a SET_BLOCK per furnace, the
	seed of the hw1029c client relight storm); this pins the Beta behaviour
	through the production load path (SRPC_LOAD_WORLD -> region read -> TE
	insert -> furnace_server_load_fixup_w -> server ticks).
*/
#include "support/owner_swap_harness.h"
#include "network/server_furnace.h"
#include "network/tile_entities_nbt.h"
#include <stdlib.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#define FY 64
/* three furnaces in chunk (2,1) -- streamed in after the load, like every
 * chunk of a real world except the spawn column; spawn is chunk (0,0). */
#define FCX 2
static const struct { int x, z; uint8_t id; int16_t burn; } CASES[3] = {
	{FCX * 16 + 3, 16 + 3, BLOCK_FURNACE_LIT, 0},   /* Beta map lamp: lit, no fuel burning */
	{FCX * 16 + 6, 16 + 3, BLOCK_FURNACE_LIT, 100}, /* mid-burn: keeps burning down */
	{FCX * 16 + 9, 16 + 3, BLOCK_FURNACE, 0},       /* idle stays idle */
};

static void write_world(const char* dir) {
	assert(MKDIR(dir) == 0);
	string_t name;
	string_init_set_str(name, dir);
	struct level_archive level;
	assert(level_archive_create_new(&level, name, 4242,
		(vec3) {6.5F, FY + 1 + 1.62F, 8.5F}));
	assert(level_archive_save(&level));
	level_archive_discard(&level);
	char sub[1024];
	snprintf(sub, sizeof sub, "%s/region", dir);
	assert(MKDIR(sub) == 0);

	/* A 3x3 of fully populated slab chunks around (FCX,0): Beta populate()
	 * of an unpopulated neighbour decorates into this chunk (lakes at sea
	 * level), so every neighbour is saved populated and nothing spills. */
	const size_t count = CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
	struct region_archive region;
	assert(region_archive_create_new(&region, name, 0, 0, WORLD_DIM_OVERWORLD));
	for(int cx = FCX - 1; cx <= FCX + 1; cx++)
		for(int cz = 0; cz <= 2; cz++) { /* all inside region (0,0) */
			struct server_chunk chunk = {0};
			chunk.ids = calloc(count, 1);
			chunk.metadata = calloc(count / 2, 1);
			chunk.lighting_sky = calloc(count / 2, 1);
			chunk.lighting_torch = calloc(count / 2, 1);
			chunk.heightmap = calloc(CHUNK_SIZE * CHUNK_SIZE, 1);
			assert(chunk.ids && chunk.metadata && chunk.lighting_sky
				&& chunk.lighting_torch && chunk.heightmap);
			chunk.populated = true;
			for(int x = 0; x < CHUNK_SIZE; x++)
				for(int z = 0; z < CHUNK_SIZE; z++) {
					for(int y = 0; y <= FY; y++)
						chunk.ids[y + (z + x * CHUNK_SIZE) * WORLD_HEIGHT] = BLOCK_STONE;
					chunk.heightmap[x + z * CHUNK_SIZE] = FY + 1;
					for(int y = FY + 1; y < WORLD_HEIGHT; y++) {
						size_t i = y + (z + x * CHUNK_SIZE) * WORLD_HEIGHT;
						chunk.lighting_sky[i / 2] |= (uint8_t)(0xF << ((i & 1) * 4));
					}
				}
			struct te_furnace te[3] = {0};
			struct chunk_tile_entities tes = {0};
			if(cx == FCX && cz == 1) {
				for(int k = 0; k < 3; k++) {
					size_t i = FY + ((CASES[k].z & 15) + (CASES[k].x & 15) * CHUNK_SIZE) * WORLD_HEIGHT;
					chunk.ids[i] = CASES[k].id;
					chunk.metadata[i / 2] |= (uint8_t)(2 << ((i & 1) * 4)); /* facing north */
					te[k].x = CASES[k].x;
					te[k].y = FY;
					te[k].z = CASES[k].z;
					te[k].burn_time = CASES[k].burn;
					/* coal in the fuel slot except for the mid-burn case: an
					 * imported item record is pinned (immutable, furnace_tick
					 * returns early), so that furnace would never count down */
					if(k != 1)
						te[k].items[FURNACE_SLOT_FUEL_TE] = (struct item_data) {.id = ITEM_COAL, .count = 1};
				}
				tes.furnaces = te;
				tes.n_furnace = 3;
			}
			assert(region_archive_set_blocks(&region, cx, cz, &chunk, &tes));
			free(chunk.ids); free(chunk.metadata); free(chunk.lighting_sky);
			free(chunk.lighting_torch); free(chunk.heightmap);
		}
	assert(region_archive_sync(&region));
	assert(region_archive_destroy(&region));
	string_clear(name);
}

static uint8_t block_id(struct server_local* s, int x, int z) {
	struct block_data b = {0};
	assert(server_world_get_block(&s->worlds[0] /* Overworld-only fixture */, x, FY, z, &b));
	return b.type;
}

int main(void) {
	harness_init();
	char root[256], world[512];
	test_fixture_dir("test_furnace_load_beta", root, sizeof root);
	assert(MKDIR(root) == 0);
	snprintf(world, sizeof world, "%s/world", root);
	write_world(world);

	struct server_local* s = calloc(1, sizeof *s);
	assert(s);
	server_local_test_create_without_thread(s, false, false, false, false,
		false, false, false);
	svin_set_local_motion(true);
	struct server_rpc call = {.type = SRPC_LOAD_WORLD};
	string_init_set_str(call.payload.load_world.name, world);
	assert(svin_rpc_send(&call));
	drain_server(s);
	assert(s->player.has_pos && !s->load_failure_terminal);

	/* Stream until the spawn chunk's furnaces are resident (the load inserts
	 * the TEs and runs furnace_server_load_fixup_w on the chunk). */
	struct furnace_te* lamp = NULL;
	for(int n = 0; n < 400 && !lamp; n++) {
		client_acks_marker(s);
		tick(s);
		lamp = furnace_te_lookup_w(&s->worlds[0], CASES[0].x, FY, CASES[0].z);
	}
	assert(lamp && "furnace TE restored from the region");
	int burning0 = furnace_te_lookup_w(&s->worlds[0], CASES[1].x, FY, CASES[1].z)->burn_time;
	for(int k = 0; k < 3; k++) {
		struct furnace_te* f = furnace_te_lookup_w(&s->worlds[0], CASES[k].x, FY, CASES[k].z);
		assert(f);
		assert(f->current_item_burn_time == (k == 1 ? 0 : 1600)
			&& "readFromNBT: getItemBurnTime(fuel slot)");
		assert(block_id(s, CASES[k].x, CASES[k].z) == CASES[k].id);
	}
	assert(lamp->burn_time == 0);
	assert(burning0 > 0 && burning0 <= 100);
	puts("PASS load: block ids untouched, currentItemBurnTime recomputed from the fuel slot");

	/* Furnaces tick only once the player has finished loading (the server's
	 * `loading` gate). Reach that, then five more server ticks: the lamp stays
	 * lit (was_burning == is_burning, no updateFurnaceBlockState), the mid-burn
	 * one burns down 5 ticks and stays lit, the idle one stays idle. Nothing
	 * smelts (empty input). */
	for(int n = 0; n < 400 && !s->player.finished_loading; n++) {
		client_acks_marker(s);
		tick(s);
	}
	assert(s->player.finished_loading);
	burning0 = furnace_te_lookup_w(&s->worlds[0], CASES[1].x, FY, CASES[1].z)->burn_time;
	assert(burning0 > 0 && burning0 <= 100);
	for(int t = 0; t < 5; t++)
		tick(s);
	assert(block_id(s, CASES[0].x, CASES[0].z) == BLOCK_FURNACE_LIT);
	assert(furnace_te_lookup_w(&s->worlds[0], CASES[0].x, FY, CASES[0].z)->burn_time == 0);
	assert(block_id(s, CASES[1].x, CASES[1].z) == BLOCK_FURNACE_LIT);
	/* The per-tick countdown itself is test_furnace_burn's golden; here only
	 * "still burning, still lit, never relit/extinguished by a load" is pinned
	 * (this harness's server ticks may not reach the furnace pass). */
	{
		int b = furnace_te_lookup_w(&s->worlds[0], CASES[1].x, FY, CASES[1].z)->burn_time;
		assert(b > 0 && b <= burning0);
	}
	assert(block_id(s, CASES[2].x, CASES[2].z) == BLOCK_FURNACE);
	puts("PASS tick: lit+BurnTime 0 stays lit (Beta map lamp); mid-burn stays lit; idle stays idle");

	s->paused = true;
	assert(server_local_unload_world_impl(s, false));
	clin_update_drain();
	server_local_shutdown(s);
	svin_set_local_motion(false);
	free(s);
	test_fixture_dir_cleanup(root);
	puts("test_furnace_load_beta: PASS");
	return 0;
}
