/* Copyright (c) 2026 CaveWii_v2 contributors
 * Explosion vs knockBack vertical impulse parity (lane C, 2026-09-27).
 *
 * Beta 1.7.3 (client Jad decompile,
 * ../mcb173-retromcp/minecraft/src_original/net/minecraft/src/):
 *   - Explosion.doExplosionA :100-103: for each entity in range
 *     attackEntityFrom(exploder, dmg) THEN motionX/Y/Z += dir *
 *     v (the density factor) -- the blast impulse is added AFTER the damage call.
 *   - EntityLiving.attackEntityFrom :336-346: a fresh hit from a non-null
 *     attacker (a creeper exploder) runs knockBack first.
 *   - EntityLiving.knockBack :392-405: motion /= 2; motionX/Z -= d/|d| *
 *     0.4F; motionY += 0.4F; if(motionY > 0.4F) motionY = 0.4F.
 *   => a mob at rest hit by a creeper blast ends with motionY =
 *      min(0/2 + 0.4, 0.4) + dirY * v = 0.4 + dirY * v -- the blast's
 *      vertical share survives ON TOP of the knockBack pop, up or down.
 *
 * (RED history: the engine used to defer the knockBack vertical pop to the
 * mob's next tick and compose it from the pre-force velocity, which already
 * carried the blast's share -- an upward share was lost, a downward one was
 * read as a fall. EXPLOSION-KB bridged it with knockback_extra_vy; mob motion
 * units S2 retires both: knockBack is composed AT THE HIT on the
 * authoritative doubles, then the blast adds its impulse, so the result is
 * checked right after the blast, byte-exact.)
 *
 * Cases (one per process, argv[1]):
 *   up    zombie 3 blocks off and 2 blocks ABOVE a creeper blast: right after
 *         the blast motionY == 0/2 + (double)0.4F + dirY*v (> 0.4)
 *   down  zombie 2 blocks BELOW: motionY == (double)0.4F + dirY*v (< 0.4)
 *   tnt   the same geometry with a null exploder (TNT): no knockBack, the
 *         impulse alone is the vertical motion */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define MKDIR(p) _mkdir(p)
#define GETPID() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define GETPID() getpid()
#endif

#include <m-lib/m-string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/mob_motion.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/monsters_object.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static volatile int drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static char g_tmp[96];

int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : "up";
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	int n = snprintf(g_tmp, sizeof g_tmp, "test_explosion_kbv_tmp_%d",
					 (int)GETPID());
	assert(n > 0 && (size_t)n < sizeof g_tmp);
	MKDIR(g_tmp);
	char region[128];
	snprintf(region, sizeof region, "%s/region", g_tmp);
	MKDIR(region);
	string_t level;
	string_init_set_str(level, g_tmp);
	static struct server_local s;
	memset(&s, 0, sizeof(s));
	/* Overworld-only fixture: worlds[0] named explicitly (world alias gate). */
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	s.scheduled_tick_budget = 128;
	s.rand_src.seed = UINT32_C(0xB1A57B1D);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = s.player.finished_loading = true;
	s.player.x = 60.5;
	s.player.y = 4.5 + 30.0; /* high above: in range for AI, out of the blast */
	s.player.z = 40.5;
	for(int cx = 0; cx <= 6; cx++)
		for(int cz = -1; cz <= 3; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}

	bool tnt = !strcmp(which, "tnt");
	float dy = strcmp(which, "down") ? 2.0F : -2.0F;
	vec3 boom = {60.5F, 10.0F, 8.5F};
	struct entity* creeper = NULL;
	if(!tnt) {
		creeper = server_local_spawn_monster_at(boom, MONSTER_CREEPER, &s);
		assert(creeper);
		glm_vec3_zero(creeper->vel);
		mob_motion_adopt_floats(creeper, &creeper->data.monster.motion_d);
		server_local_refresh_entity_membership(&s, creeper);
	}
	vec3 zpos = {63.5F, 10.0F + dy, 8.5F};
	struct entity* zom = server_local_spawn_monster_at(zpos, MONSTER_ZOMBIE, &s);
	assert(zom && zom->health == 20);
	glm_vec3_zero(zom->vel);
	mob_motion_adopt_floats(zom, &zom->data.monster.motion_d);
	server_local_refresh_entity_membership(&s, zom);

	/* Beta terms for size 3 (size2 6), open air (density 1), on the victim's
	 * DOUBLE position (the fixture's float spawn widens exactly). */
	const struct mob_motion* zmd = &zom->data.monster.motion_d;
	double ddx = zmd->pos[0] - (double)boom[0];
	double ddy = zmd->pos[1] - (double)boom[1];
	double ddz = zmd->pos[2] - (double)boom[2];
	double dist = (double)(float)sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
	double var13 = dist / 6.0;
	double var41 = (1.0 - var13) * 1.0;
	double impulse_y = ddy / dist * var41;

	server_world_begin_update(&s.worlds[0]);
	if(tnt)
		assert(server_world_try_explode_by(&s.worlds[0], NULL, boom, 3.0F));
	else
		assert(server_world_try_explode_by(&s.worlds[0], creeper, boom, 3.0F));
	double want = tnt ? 0.0 + impulse_y : 0.0 / 2.0 + (double)0.4F + impulse_y;
	printf("after blast: motion (%.9f, %.17g, %.9f) kb=%d want_y %.17g "
		   "(impulse_y %.17g)\n",
		   zmd->motion[0], zmd->motion[1], zmd->motion[2],
		   zom->data.monster.knockback_time, want, impulse_y);
	bool ok = fabs(zmd->motion[1] - want) < 1.0e-12;
	printf("%s %s: motionY after the blast == %s\n", ok ? "ok  " : "FAIL",
		   which, tnt ? "the impulse alone (no knockBack)"
					  : "0.4F + dirY*var41 (Beta order, composed at the hit)");

	drain_run = 0;
	thread_join(&drain);
	return ok ? 0 : 1;
}
