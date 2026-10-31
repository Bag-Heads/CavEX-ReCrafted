/* Config lever through the production server update, RPC queue, and client. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "support/b1_bind_client.h"
#include "block/blocks.h"
#include "config.h"
#include "game/game_state.h"
#include "game/client_world_context.h"
#include "game/weather_force_config.h"
#include "graphics/render_weather.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_weather.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "platform/time.h"
#include "worldgen/worldgen.h"

void server_local_test_reset_weather_set_send_count(void);
unsigned server_local_test_weather_set_send_count(void);

static struct server_local s;
static volatile int draining = 1;
static void* drain_client(void* unused) {
	(void)unused;
	while(draining) {
		clin_update_drain();
		thread_msleep(1);
	}
	clin_update_drain();
	return NULL;
}

int main(void) {
	struct config c = {0};
	c.root = json_parse_string("{\"debug\":{\"weather_force\":\"rain\","
		"\"weather_force_int\":2}}");
	assert(c.root && clin_init());
	/* Split-dims B1: world-scoped RPCs reach the client only through an
	 * installed world context -- a real client gets it from CRPC_WORLD_RESET.
	 * This fixture never resets a client world, so it installs the single
	 * unstamped context directly. */
	assert(client_ctx_registry_install(clin_world_contexts(), 0, 0, 0));
	dict_entity_init(gstate.entities);
	blocks_init();
	server_local_init(&s, false, false, false, false, false, false, false);
	assert(weather_force_from_config(&c, &s.weather.debug_force) == 1);
	assert(s.weather.debug_force == 1); /* string wins over integer alias */
	s.rand_src.seed = 0x51DEB001u;
	s.world.dimension = WORLD_DIM_OVERWORLD;
	server_weather_seed(&s); /* same call used by world load */
	assert(s.weather.raining && s.weather.rain_time == 24000);

	/* Find a generated, sky-exposed, rain-enabled cell. */
	string_t name;
	string_init_set_str(name, "test_weather_force_e2e_tmp");
	server_world_create(&s.world, name, WORLD_DIM_OVERWORLD, 424242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	b1_bind_client_world(&s.worlds[0]);
	int rain_x = 0, rain_z = 0;
	bool found = false;
	for(int x = -256; x <= 256 && !found; x += 16)
		for(int z = -256; z <= 256 && !found; z += 16)
			if(precip_biome_enable_rain(worldgen_biome_at(s.world.generator, x, z))) {
				rain_x = x; rain_z = z; found = true;
			}
	assert(found);
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&s.world, rain_x >> 4, rain_z >> 4, &chunk));
	assert(chunk);
	assert(world_can_block_see_sky(&s, rain_x, 100, rain_z));

	s.player.finished_loading = true;
	s.player.has_pos = true;
	uint8_t ground_height = 0;
	assert(server_world_get_height(&s.world, rain_x, rain_z, &ground_height));
	s.player.x = rain_x + 0.5;
	s.player.y = ground_height + 1;
	s.player.z = rain_z + 0.5;
	s.player.health = 160;
	server_local_test_reset_weather_set_send_count();
	struct thread drain;
	assert(thread_create(&drain, drain_client, NULL, 64));
	for(int tick = 0; tick < 100; tick++) {
		server_local_test_update_once(&s);
		/* G9's real server loop flushes owned pending publication outside
		 * update_once. Forward that boundary in this threadless fixture. */
		(void)clin_rpc_flush_pending();
	}
	ptime_t flush_start = time_get();
	while(!clin_rpc_flush_pending()) {
		assert(time_diff_s(flush_start, time_get()) < 10);
		thread_msleep(1);
	}
	draining = 0;
	thread_join(&drain);
	assert(server_local_test_weather_set_send_count() > 0);
	assert(gstate.weather_raining);
	fprintf(stderr, "client rain strength %.3f; sends %u\n",
		(double)gstate.weather_rain_strength,
		server_local_test_weather_set_send_count());
	assert(gstate.weather_rain_strength >= 0.99f);
	assert(world_can_block_be_rained_on(&s, rain_x, 100, rain_z));
	puts("test_weather_force_end_to_end: PASS");
	string_clear(name);
	config_destroy(&c);
	return 0;
}
