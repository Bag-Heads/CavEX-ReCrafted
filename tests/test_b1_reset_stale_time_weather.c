/* Copyright (c) 2026 CaveWii_v2 contributors
 *
 * B-NOW (lane B): stale TIME_SET / WEATHER_SET after a client world reset,
 * driven by the REAL producer into the real consumer.
 *
 * Producer: server_local.c::server_local_sleep_tick, the night-skip commit
 * (Beta World.tick:1740-1752 -> wakeUpAllPlayers -> stopPrecipitation), which
 * publishes CRPC_WEATHER_SET (rain cleared) and CRPC_TIME_SET (next dawn + 1)
 * with blocking sends. The same message shapes are published by the per-tick
 * weather broadcast in server_local_update and by every other TIME_SET site
 * (pause toggle, load tail) -- none of them stamp.
 *
 * Sequence (the same real FIFO inversion as test_b1_reset_stale_terrain): the
 * sleeping session's server has admitted its night-skip clock/weather when the
 * client tears down on its own thread (clin_force_world_reset) and the next
 * session installs synchronously (clin_force_world_reset_login) and receives
 * its OWN clock and weather through clin_process_direct (remote Packet4 /
 * Packet70 shape). Then the dead session's queue drains.
 *
 * GREEN (asserts): both stale messages reach the consumer, one live payload
 * slot throughout, the successor context is never replaced.
 * RED (b1r_red, WILL_FAIL): each stale message carries its producing world's
 * stamp, and neither moves the successor's clock or weather. */
#include "test_b1_reset_harness.h"

#include "block/blocks_data.h"
#include "game/difficulty.h"

void server_local_test_set_difficulty(int d);

#define SUCCESSOR_TIME 777u

int main(int argc, char** argv) {
	b1r_mode(argc, argv);
	b1r_client_start();
	b1r_capture(CRPC_TIME_SET);
	b1r_capture(CRPC_WEATHER_SET);

	static struct b1r_server srv;
	b1r_server_create(&srv, "test_b1_reset_stale_time_weather");
	struct server_world* w = b1r_server_load(&srv);
	const uint8_t old_index = w->world_index;
	const uint32_t old_gen = w->generation;
	b1r_drain();
	assert(client_ctx_registry_current(b1r_reg, 0));
	b1r_assert_single_payload();

	/* Night, raining, the host fully asleep in its bed (the recipe of
	 * test_weather_timers_env's sleep case), peaceful so performSleepSpawning
	 * cannot cancel the skip. */
	struct server_local* s = &srv.s;
	server_local_test_set_difficulty(DIFFICULTY_PEACEFUL);
	assert(server_world_set_block_silent(w, 8, 5, 8,
		(struct block_data) {.type = BLOCK_BED}));
	s->player.has_pos = true;
	s->player.finished_loading = true;
	s->player.sleeping = true;
	s->player.sleep_timer = 100;
	s->player.sleep_x = 8;
	s->player.sleep_y = 5;
	s->player.sleep_z = 8;
	s->world_time = 13000;
	s->weather.raining = true;
	s->weather.thundering = true;
	s->weather.rain_time = 4242;
	s->weather.thunder_time = 1337;
	/* The per-tick skylight cache (w2) still holds the load-time daytime
	 * value; recompute from the night clock set above. */
	s->skylight_cached = false;
	server_local_sleep_tick(s);
	assert(!s->weather.raining && s->world_time == 24001
		   && "the real night-skip committed (its RPCs are now queued)");

	clin_force_world_reset();
	clin_force_world_reset_login(WORLD_DIM_OVERWORLD, 0);
	b1r_assert_single_payload();
	struct client_world_context* succ = client_ctx_registry_current(b1r_reg, 0);
	assert(succ);
	/* The successor's own clock and weather (remote path, unstamped exactly
	 * as the remote producers publish them today). */
	clin_process_direct(&(struct client_rpc) {.type = CRPC_TIME_SET,
		.payload.time_set = SUCCESSOR_TIME});
	clin_process_direct(&(struct client_rpc) {.type = CRPC_WEATHER_SET,
		.payload.weather_set = {.raining = true, .rain_strength = 1.0F}});
	assert(gstate.world_time == SUCCESSOR_TIME && gstate.weather_raining);

	unsigned time_before = 0, weather_before = 0;
	(void)b1r_captured(CRPC_TIME_SET, &time_before);
	(void)b1r_captured(CRPC_WEATHER_SET, &weather_before);
	b1r_drain();
	unsigned time_seen = 0, weather_seen = 0;
	const struct client_rpc* time_msg = b1r_captured(CRPC_TIME_SET, &time_seen);
	const struct client_rpc* weather_msg
		= b1r_captured(CRPC_WEATHER_SET, &weather_seen);
	assert(time_seen == time_before + 1 && weather_seen == weather_before + 1
		   && "the dead session's clock + weather reached the real consumer");
	assert(time_msg->payload.time_set == 24001);
	assert(!weather_msg->payload.weather_set.raining);
	assert(client_ctx_registry_current(b1r_reg, 0) == succ);
	b1r_assert_single_payload();

	struct server_world stamp_world = {.world_index = old_index};
	printf("stale TIME_SET stamp {index %u, gen %u}, WEATHER_SET stamp {index "
		   "%u, gen %u}; producing world {index %u, gen %u}; successor clock "
		   "now %llu, raining %d\n",
		   time_msg->world_index, (unsigned)time_msg->world_generation,
		   weather_msg->world_index, (unsigned)weather_msg->world_generation,
		   old_index, (unsigned)old_gen,
		   (unsigned long long)gstate.world_time, (int)gstate.weather_raining);
	b1r_red(b1r_stamped_by(time_msg, &stamp_world, old_gen),
		"server_local.c::server_local_sleep_tick (CRPC_TIME_SET; likewise every "
		"TIME_SET site: load tail, SRPC_TOGGLE_PAUSE, demos)",
		"the night-skip TIME_SET carries its producing world's stamp");
	b1r_red(b1r_stamped_by(weather_msg, &stamp_world, old_gen),
		"server_local.c::server_local_sleep_tick (CRPC_WEATHER_SET; likewise the "
		"per-tick weather broadcast in server_local_update)",
		"the night-skip WEATHER_SET carries its producing world's stamp");
	b1r_red(gstate.world_time == SUCCESSOR_TIME,
		"server_local.c::server_local_sleep_tick (CRPC_TIME_SET)",
		"a dead session's TIME_SET never moves the successor's clock");
	b1r_red(gstate.weather_raining,
		"server_local.c::server_local_sleep_tick (CRPC_WEATHER_SET)",
		"a dead session's WEATHER_SET never clears the successor's rain");

	clin_test_set_consume_observer(NULL, NULL);
	b1r_server_destroy(&srv);
	return b1r_finish("test_b1_reset_stale_time_weather");
}
