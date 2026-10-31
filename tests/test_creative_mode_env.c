/*
	CREATIVE MODE (net-new addition, maintainer exception 2026-08-25;
	docs/creative_mode.md). Drives the real server damage funnel through
	server_local_damage_player_result / server_local_hurt_player /
	server_local_set_player_health / server_local_test_apply_player_hazards:

	  C1  creative ON: flat damage (the arrow/fall/fire/drowning/cactus/
	      explosion/suffocation path) is REJECTED -- health untouched, and
	      the false return means both damage callers roll back i-frame/armor;
	  C2  creative ON: healing still passes (and clamps at MAX);
	  C3  creative ON: a plain (non-void) direct health-decrease write via
	      the PUBLIC server_local_set_player_health is still rejected -- the
	      general damage API stays blocked in creative;
	  C4  creative OFF on the same fixture: the identical damage call lands
	      -- the DEFAULT (survival) path is byte-identical to before this
	      feature existed; the flag only ever adds early rejects;
	  C5  creative ON: mob melee (server_local_hurt_player) is rejected too
	      -- no health loss, no knockback velocity/pop applied;
	  C6  creative ON, maintainer ruling 2026-09-03 (supersedes phase-1's
	      "void included" wording): the ONE exception is the void kill
	      [740c583 Entity.onEntityUpdate: posY < -64 -> kill()] -- driven
	      through the real point-sampled hazard sweep
	      (server_local_test_apply_player_hazards) with the player below
	      VOID_KILL_Y, the ordinary 4-point living hit lands even though
	      creative_mode is still true;
	  C7  creative OFF (survival) equivalence: the identical below-VOID_KILL_Y
	      hazard sweep applies the same living hit in survival.

	The no-consume gates (block place snapshot/restore, bow arrow, egg/
	snowball, boat) live behind static RPC-pump paths that need a full world
	load to drive; they are covered by the Dolphin capture proof
	(creative-mode-*.png on :8099), not here.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "entity/combat.h" // MONSTER_ZOMBIE / mob_melee_damage (C5's attacker)
#include "entity/entity.h" // struct entity (C5's synthetic attacker)
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h" /* MAX_PLAYER_HEALTH / VOID_KILL_Y (PLAYER x8 scale) */
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "platform/thread.h"

static volatile int drain_run = 1;
static void* drain_client(void* unused) {
	(void)unused;
	while(drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

int main(void) {
	blocks_init();
	items_init();
	assert(clin_init());
	assert(svin_init());

	struct thread drain;
	thread_create(&drain, drain_client, NULL, 64);

	struct server_local server;
	memset(&server, 0, sizeof(server));
	rand_gen_seed(&server.rand_src);
	server.player.health = MAX_PLAYER_HEALTH;
	server.player.has_pos = true;
	server.player.finished_loading = true;

	/* C1: creative rejects flat damage. damage_player_result returns
	 * "survived" (true here) with accepted=false -- the hit was never
	 * committed -- and health is untouched across repeated hits (the
	 * false return from set_player_health makes the caller roll back
	 * i-frame/armor state, so no i-frame drift either). */
	server.creative_mode = true;
	for(int i = 0; i < 3; i++) {
		bool accepted = true;
		/* 8 = half a heart in PLAYER x8 scale (legal damage is a multiple of
		 * HEALTH_PER_HEART/2 -- see server_local_apply_player_armor). */
		assert(server_local_damage_player_result(&server, 8, 0.0f, 0.0f,
												 &accepted));
		assert(!accepted);
		assert(server.player.health == MAX_PLAYER_HEALTH);
	}

	/* C2: healing (a non-decrease) still passes and clamps at MAX. */
	assert(server_local_set_player_health(&server, MAX_PLAYER_HEALTH + 5));
	assert(server.player.health == MAX_PLAYER_HEALTH);

	/* C3: a plain (non-void) direct health-decrease write through the PUBLIC
	 * API is rejected too -- creative blocks the general damage surface,
	 * not just the two wrapper functions above. */
	assert(!server_local_set_player_health(&server, 0));
	assert(server.player.health == MAX_PLAYER_HEALTH);

	/* C4: same fixture, creative OFF -> the identical call lands, accepted
	 * and survived (default survival path unchanged; zeroed armor gives
	 * unmitigated damage). */
	server.creative_mode = false;
	bool accepted = false;
	assert(server_local_damage_player_result(&server, 8, 0.0f, 0.0f,
											 &accepted));
	assert(accepted);
	assert(server.player.health == MAX_PLAYER_HEALTH - 8);
	server.player.health = MAX_PLAYER_HEALTH; // reset for the remaining cases
	server.creative_mode = true;

	/* C5: mob melee (server_local_hurt_player) is rejected in creative too --
	 * no health loss AND no knockback (the false return from
	 * set_player_health_ex makes hurt_player roll back i-frame state and
	 * return before ever reaching the knockback tail). */
	{
		struct entity attacker;
		memset(&attacker, 0, sizeof(attacker));
		attacker.data.monster.id = MONSTER_ZOMBIE; // 40 dmg [combat.h]
		attacker.pos[0] = server.player.x + 1.0f;
		attacker.pos[1] = (float)server.player.y;
		attacker.pos[2] = (float)server.player.z;
		double px = server.player.x, py = server.player.y, pz = server.player.z;
		float pvx = server.player.vel_x, pvz = server.player.vel_z;
		server_local_hurt_player(&server, &attacker);
		assert(server.player.health == MAX_PLAYER_HEALTH);
		assert(server.player.x == px && server.player.y == py
			   && server.player.z == pz);
		assert(server.player.vel_x == pvx && server.player.vel_z == pvz);
	}

	/* C6: the creative exception still uses Beta's LIVING override:
	 * Entity.onEntityUpdate's strict posY < -64 calls virtual kill(), while
	 * EntityLiving.kill calls attackEntityFrom(null, 4). The retained fallback
	 * decompile has these at Entity.java:228-251 and EntityLiving.java:815-818;
	 * independent mcp_spout 790e9eff agrees at 287-308 and 913-916. */
	server.player.hurt_resist_time = server.player.last_damage = 0;
	server.player.damage_remainder = 0;
	server.player.y = VOID_KILL_Y - 1.0;
	assert(server_local_test_apply_player_hazards(
		&server, false, false, false, false));
	assert(server.player.health == MAX_PLAYER_HEALTH - 2 * HEALTH_PER_HEART);
	short creative_void_health = server.player.health;
	assert(server_local_test_apply_player_hazards(
		&server, false, false, false, false));
	assert(server.player.health == creative_void_health);

	/* C7: survival equivalence for the same living void-damage path. Reset the
	 * resistance state so the identical raw four-point hit can land. */
	server.creative_mode = false;
	server.player.health = MAX_PLAYER_HEALTH;
	server.player.hurt_resist_time = server.player.last_damage = 0;
	server.player.damage_remainder = 0;
	server.player.y = VOID_KILL_Y - 1.0;
	assert(server_local_test_apply_player_hazards(
		&server, false, false, false, false));
	assert(server.player.health == MAX_PLAYER_HEALTH - 2 * HEALTH_PER_HEART);

	drain_run = 0;
	thread_join(&drain);
	printf("test_creative_mode_env: PASS\n");
	return 0;
}
