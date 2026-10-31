/* next-gaps-0924/w5 -- the debug.sleep_spawn_stress hardware fixture (docs/intent/w5-sleep-spawn-stress-fixture.md).
 *
 * Driven through the REAL server update: once armed, the fixture builds its unlit field + bed, and every
 * SLEEP_SPAWN_STRESS_PERIOD (300) ticks puts the host back to bed at midnight one tick short of fully asleep,
 * so the following tick runs Beta's completing-sleep path (World.tick:1740-1752 -> performSleepSpawning,
 * SpawnerAnimals.java:169-246). This test proves the fixture really exercises that tick: completed cycles
 * accumulate, every one is counted by the fixture, and the idle host survives (monsters are reaped and the
 * clock moved to noon right after each measured tick).
 */
#include "support/split_residency_fixture.h"
#include "entity/combat.h"
#include "item/items.h"
#include <stdio.h>

static volatile int draining = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(draining) { clin_drain_discard(); thread_msleep(1); }
	clin_drain_discard();
	return NULL;
}

int main(void) {
	assert(clin_init());
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	struct server_local* s = split_fixture();
	s->rand_src = (struct random_gen) {.seed = 97531};
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	s->far_gen_budget_ms = INT_MAX;
	s->far_view_distance = 0;
	s->player.active_inventory = &s->player.inventory;
	s->input_ack_pending = 1;
	s->player.finished_loading = true;
	for(unsigned load = 0; load < 64; load++)
		server_local_test_update_once(s);
	s->input_ack_pending = 0;

	// Arm exactly as the post-load hook does for the card key (defer elapses on the next tick).
	s->sleep_spawn_stress = true;
	s->sleep_stress_defer = 1;
	int completed = 0;
	for(int t = 0; t < 3 * 300 + 10; t++) {
		bool was_asleep = s->player.sleeping && s->player.sleep_timer == 99;
		server_local_test_update_once(s);
		if(was_asleep && !s->player.sleeping)
			completed++; // the completing tick ran (its Beta clock outcome is pinned by test_sleep_spawn_golden)
		if(s->player.health < MAX_PLAYER_HEALTH)
			fprintf(stderr, "  t=%d health=%d cause=%u pos=(%.2f,%.2f,%.2f) sleeping=%d\n", t, s->player.health,
				(unsigned)s->player_damage_cause, s->player.x, s->player.y, s->player.z, s->player.sleeping);
		assert(s->player.health > 0 && "the fixture must keep the host alive");
	}
	int cycles = server_local_sleep_stress_cycles(s), spawns = server_local_sleep_stress_spawns(s);
	printf("sleep_spawn_stress: %d completing sleep ticks observed, fixture counted %d cycles (%d sleep spawns)\n",
		completed, cycles, spawns);
	int failures = 0;
	if(completed < 2 || cycles != completed) { failures++; fprintf(stderr, "RED fixture did not complete and count >= 2 sleeps\n"); }

	draining = 0;
	thread_join(&drain);
	split_destroy(s);
	if(failures) {
		fprintf(stderr, "test_sleep_spawn_stress_fixture: %d FAILED\n", failures);
		return 1;
	}
	printf("test_sleep_spawn_stress_fixture: all PASS\n");
	return 0;
}
