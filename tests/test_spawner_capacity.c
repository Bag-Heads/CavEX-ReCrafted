/* Spawner capacity regression: real terrain/population, buffer-backed world,
 * and the production TE upsert/lookup. No lighting/fluid simulation claim.
 * WorldGenDungeons.generate creates a MobSpawner TE for every spawner block;
 * a resident store must not silently drop that callback at row 65. */
#include <assert.h>
#include <stdio.h>

#include "network/server_local_state.h"
#include "network/server_spawner.h"
#include "worldgen/worldgen.h"
#include "worldgen/populate.h"

#define POPULATE_SIDE 65
#define WIDTH (POPULATE_SIDE + 1)
#define ORIGIN (-32)
static struct server_chunk chunks[WIDTH][WIDTH];
static struct server_local session;
static int placements, upserts, dropped;

static uint8_t* cell(int x, int y, int z) {
	int cx = (x >> 4) - ORIGIN, cz = (z >> 4) - ORIGIN;
	if(y < 0 || y >= WORLD_HEIGHT || cx < 0 || cz < 0
	   || cx >= WIDTH || cz >= WIDTH)
		return NULL;
	return &chunks[cx][cz].ids[y + ((z & 15) + (x & 15) * 16) * WORLD_HEIGHT];
}

static uint8_t get(void* ctx, int x, int y, int z) {
	(void)ctx;
	uint8_t* p = cell(x, y, z);
	return p ? *p : 0;
}

static void set(void* ctx, int x, int y, int z, uint8_t id, uint8_t meta) {
	(void)ctx;
	(void)meta;
	uint8_t* p = cell(x, y, z);
	if(!p) return;
	if(*p == 52 && id != 52)
		spawner_te_free_w(&session.worlds[0], x, y, z);
	if(id == 52 && *p != 52) placements++;
	*p = id;
}

static void set_spawner(void* ctx, int x, int y, int z, const char* mob) {
	(void)ctx;
	assert(get(NULL, x, y, z) == 52);
	if(spawner_te_upsert_w(&session.worlds[0], x, y, z, mob)) upserts++;
	else dropped++;
}

int main(void) {
	session.worlds[0].session = &session;
	session.worlds[0].generation = 1;
	if(!server_local_te_tables_init(&session)) /* IMP-TE-CAPS: growable slabs, all free */
		abort();
	struct worldgen* wg = worldgen_create(4242, WORLD_DIM_OVERWORLD);
	assert(wg);
	for(int x = 0; x < WIDTH; x++)
		for(int z = 0; z < WIDTH; z++)
			assert(worldgen_generate_chunk(wg, &chunks[x][z], x + ORIGIN, z + ORIGIN));
	struct wgen_world w = {.get = get, .set = set, .set_spawner = set_spawner};
	for(int x = ORIGIN; x < ORIGIN + POPULATE_SIDE; x++)
		for(int z = ORIGIN; z < ORIGIN + POPULATE_SIDE; z++)
			worldgen_populate(4242, x, z, &w,
				worldgen_biome_at(wg, x * 16 + 16, z * 16 + 16),
				worldgen_tree_density(wg, x * 16, z * 16));
	int live = 0, missing = 0, rows = 0;
	for(int x = ORIGIN * 16; x < (ORIGIN + WIDTH) * 16; x++)
		for(int z = ORIGIN * 16; z < (ORIGIN + WIDTH) * 16; z++)
			for(int y = 0; y < WORLD_HEIGHT; y++)
				if(get(NULL, x, y, z) == 52) {
					live++;
					if(!spawner_te_lookup_w(&session.worlds[0], x, y, z)) missing++;
				}
	for(int i = 0; i < MAX_SPAWNER_TES; i++)
		if(session.spawner_te_pos[i].y >= 0) rows++;
	printf("spawner capacity: chunks=%d placements=%d upserts=%d dropped=%d live=%d rows=%d missing=%d capacity=%d\n",
		POPULATE_SIDE * POPULATE_SIDE, placements, upserts, dropped, live, rows, missing, MAX_SPAWNER_TES);
	fflush(stdout);
	assert(live > 64);
	assert(placements == upserts && dropped == 0);
	assert(live == rows && missing == 0);
	assert(sizeof(struct complex_block_pos) + sizeof(struct spawner_te)
		+ sizeof(uint32_t) + sizeof(uint8_t) == 43);
	for(int x = 0; x < WIDTH; x++)
		for(int z = 0; z < WIDTH; z++) server_chunk_arrays_free(&chunks[x][z]);
	worldgen_destroy(wg);
	puts("test_spawner_capacity: PASS (all generated spawner blocks retain TEs)");
	return 0;
}
