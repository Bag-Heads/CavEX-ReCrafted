/* Copyright (c) 2026 CaveWii_v2 contributors */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "network/server_local_state.h"
#include "network/server_local.h"
#include "network/client_interface.h"
#include "game/game_state.h"

/* L211: with no rider the relation-aware wrapper must preserve every byte of
 * the existing SP dwell machine (including IEEE-754 ramp and cooldown pin).
 * Expected ordering: RetroMCP client EntityPlayerSP.java:39-77. */
static void test_unmounted_portal_dwell_equiv(void) {
	struct server_local* local = calloc(1, sizeof(*local));
	assert(local);
	dict_entity_init(local->entities);
	struct server_player expected = local->player;
	local->player.time_until_portal = expected.time_until_portal = 20;
	for(unsigned tick = 0; tick < 400; tick++) {
		server_player_portal_tick(&expected);
		server_local_actor_portal_tick(local, &local->player);
		clin_update_drain();
		/* Contact / leave / recontact exercises ramp, decay and arrival pin. */
		if(tick < 150 || tick >= 200) {
			server_player_set_in_portal(&expected);
			server_player_set_in_portal(&local->player);
		}
		assert(!memcmp(&local->player, &expected, sizeof expected));
	}
	dict_entity_clear(local->entities);
	free(local);
	puts("  portal: unmounted dwell wrapper is byte-identical for 400 ticks");
}

int main(void) {
	assert(clin_init());
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	test_unmounted_portal_dwell_equiv();
	dict_entity_clear(gstate.entities);
	world_destroy(&gstate.world);
	return 0;
}
