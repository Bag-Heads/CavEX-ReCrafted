/* Beta 1.7.3 BlockFire.java:119 and BlockDispenser.java:118,129;
 * RenderGlobal.java:1396. Exercise the live server paths. */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <m-lib/m-string.h>
#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_dispenser.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_weather.h"
#include "network/server_world.h"
#include "particle.h"
#include "util.h"

extern void block_fire_test_try_catch(struct server_local*, int, int, int, int, int);
extern void clin_dispenser_smoke(vec3 center, int direction);
static struct server_local s;
static struct client_rpc item_rpc, smoke_rpc;
static int item_count, smoke;
static void observe(const struct client_rpc* r, void* unused) {
	(void)unused;
	if(r->type == CRPC_SPAWN_ITEM) { item_rpc = *r; item_count++; }
	if(r->type == CRPC_SMOKE_PUFF) { smoke_rpc = *r; smoke++; }
}
static void set(int x, int y, int z, int id) {
	assert(server_world_set_block(&s.worlds[0], x, y, z,
		(struct block_data){.type = (uint8_t)id}));
}
static int get(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	return b.type;
}
int main(void) {
	char path[128];
	test_fixture_dir("fire_dispenser_parity", path, sizeof path);
	blocks_init();
	clin_init();
	string_t level;
	string_init_set_str(level, path);
	memset(&s, 0, sizeof s);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 99, true);
	s.worlds[0].session = &s;
	dict_entity_init(s.entities);
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &chunk));
	assert(server_world_load_chunk(&s.worlds[0], 0, 1, &chunk));
	clin_test_set_rpc_observer(observe, NULL);

	/* Caught tall grass under the sky burns to air; roofed grass may
	 * become fire. nextInt(age+10) is consumed before the rain query. */
	s.weather.raining = true;
	s.weather.rain_strength = s.weather.prev_rain_strength = 1.0f;
	for(int roof = 0; roof < 2; roof++) {
		int x = 6 + roof * 8, y = 70, z = 20;
		set(x, y - 1, z, BLOCK_STONE);
		set(x, y, z, BLOCK_TALL_GRASS);
		if(roof) set(x, y + 2, z, BLOCK_STONE);
		assert(world_can_block_be_rained_on(&s, x, y, z) == !roof);
		/* Search only seeds whose second draw enters Beta's fire branch. */
		uint32_t seed = 1;
		for(;; seed++) {
			struct random_gen g = {.seed = seed};
			(void)rand_gen_int(&g, 1);
			if(rand_gen_int(&g, 10) < 5) break;
		}
		s.rand_src = (struct random_gen){.seed = seed};
		block_fire_test_try_catch(&s, x, y, z, 1, 0);
		assert(get(x, y, z) == (roof ? BLOCK_FIRE : BLOCK_AIR));
	}

	/* Generic ejection: one world RNG double, then Gaussian X/Y/Z.
	 * Motion stores float only at the entity/RPC boundary. */
	s.weather.raining = false;
	set(12, 100, 12, BLOCK_DISPENSER);
	assert(dispenser_te_register(&s, 12, 100, 12) >= 0);
	struct dispenser_te* d = dispenser_te_lookup(&s, 12, 100, 12);
	assert(d);
	d->items[0] = (struct item_data){.id = ITEM_STICK, .count = 1};
	d->dispenser_random = (struct random_gen){.seed = 55};
	s.rand_src = (struct random_gen){.seed = 1};
	struct random_gen oracle = s.rand_src;
	uint32_t raw = rand_gen(&oracle);
	double speed = (double)raw / 4294967296.0 * 0.1 + 0.2;
	double vx = speed + (double)rand_gen_gaussian(&oracle) * (double)0.0075f * 6.0;
	double vy = (double)0.2f + (double)rand_gen_gaussian(&oracle) * (double)0.0075f * 6.0;
	double vz = (double)rand_gen_gaussian(&oracle) * (double)0.0075f * 6.0;
	server_local_dispenser_dispense(&s, 12, 100, 12, 5);
	assert(item_count == 1 && smoke == 1);
	assert(item_rpc.payload.spawn_item.vel[0] == (float)vx);
	assert(item_rpc.payload.spawn_item.vel[1] == (float)vy);
	assert(item_rpc.payload.spawn_item.vel[2] == (float)vz);
	assert(memcmp(&s.rand_src, &oracle, sizeof oracle) == 0);
	/* Direction +x: packed SFX 2000 direction is 5, carried as -6. */
	assert(smoke_rpc.payload.smoke_puff.intensity == -6.0f);
	/* RenderGlobal.java:1396: ten particles centered at +x, with
	 * direction offsets x=+0.01 and z jitter of +/-0.25. */
	particle_clear_all();
	particle_set_camera(smoke_rpc.payload.smoke_puff.pos);
	clin_dispenser_smoke(smoke_rpc.payload.smoke_puff.pos, 5);
	assert(particle_test_count() == 10);
	float sum_vx = 0.0f, sum_vy = 0.0f, sum_vz = 0.0f;
	for(size_t i = 0; i < 10; i++) {
		struct particle p;
		assert(particle_test_get(i, &p));
		assert(fabsf(p.pos[0] - (smoke_rpc.payload.smoke_puff.pos[0] + 0.01f)) < 0.0001f);
		assert(fabsf(p.pos[1] - smoke_rpc.payload.smoke_puff.pos[1]) <= 0.25f);
		assert(fabsf(p.pos[2] - smoke_rpc.payload.smoke_puff.pos[2]) <= 0.25f);
		sum_vx += p.vel[0];
		sum_vy += p.vel[1];
		sum_vz += p.vel[2];
	}
	assert(sum_vx > 0.1f && sum_vy < 0.0f && fabsf(sum_vz) < 0.1f);
	clin_test_set_rpc_observer(NULL, NULL);
	clin_drain_discard();
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_clear(s.entities);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	test_fixture_dir_cleanup(path);
	puts("PASS test_fire_dispenser_parity");
	return 0;
}
