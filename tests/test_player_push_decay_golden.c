/* G5 (vehicle-smoke-1004): the server-applied player push decays like Beta's
 * player motion, and a small push is never dropped.
 *
 * Beta (CFR EntityLiving.moveEntityWithHeading, the dry-land branch,
 * EntityLiving.java:440-470): after moveEntity(motion), motion *= var8 where
 *     var8 = 0.91F in the air;
 *     on the ground: 0.54600006F, or Block.slipperiness * 0.91F for the block
 *     at (floor x, floor(boundingBox.minY) - 1, floor z) when it is not air
 *     (0.6F default, Block.java:329; ice 0.98F, BlockIce.java:27).
 * There is no small-motion cutoff in 1.7.3. Every source of player motion this
 * engine carries in server_player.vel_x/vel_z is a Beta addVelocity onto that
 * motion (mob knockBack, fishing pull, the cart's +d/4 shove, entity push).
 *
 * Before G5 the engine decayed by 0.6F everywhere and, worse, zeroed any
 * |v| <= 0.01 BEFORE applying it: a cart's +d/4 shove (at most 0.0125, usually
 * less) never moved the player at all. Checked on the production
 * server_local_tick_actor_knockback against Beta's recurrence.
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "entity/entity.h"
#include "entity/combat.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100

static struct server_local s;
static volatile int g_drain_run = 1;
static unsigned failures;

static void check(bool ok, const char* what) {
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if(!ok)
		failures++;
}

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void place_player(double x, double z, bool on_ground, float vx,
						 float vz) {
	s.player.x = x;
	s.player.y = (double)(YF + 1) + (double)PLAYER_EYE_HEIGHT; /* feet on the floor */
	s.player.z = z;
	s.player.on_ground = on_ground;
	s.player.vel_x = vx;
	s.player.vel_z = vz;
}

/* Beta server (G8): EntityPlayerMP's motion decays by k each update while the
 * body stays where its CLIENT puts it (NetServerHandler.handleFlying,
 * setPositionAndRotation :172/257) -- the client integrates the push
 * (test_knockback_client_steer). G8-BLAST-VELOCITY v3/v4 re-derivation:
 * Beta's motionX/Z are DOUBLES and the float friction is widened at the
 * multiply (EntityLiving.java:486-487 `motionX *= (double)var8`), which the
 * engine now stores exactly -- so the recurrence is v = v * (double)k in
 * double, from the float-valued start (ASTRA-V2-REVIEW: double X/Z). */
static void run_case(const char* label, double x, double z, bool ground,
					 float v0x, float v0z, float k, int ticks) {
	place_player(x, z, ground, v0x, v0z);
	double ex = x, ez = z;
	double vx = (double)v0x, vz = (double)v0z;
	bool ok = true;
	for(int t = 1; t <= ticks; t++) {
		vx *= (double)k;
		vz *= (double)k;
		server_local_tick_actor_knockback(&s);
		if(fabs(s.player.x - ex) > 1e-9 || fabs(s.player.z - ez) > 1e-9
		   || s.player.vel_x != vx || s.player.vel_z != vz) { /* exact */
			printf("     %s t%d: pos (%.9f,%.9f) v (%.9f,%.9f) Beta pos "
				   "(%.9f,%.9f) v (%.9f,%.9f)\n", label, t, s.player.x,
				   s.player.z, (double)s.player.vel_x, (double)s.player.vel_z,
				   ex, ez, (double)vx, (double)vz);
			ok = false;
			break;
		}
	}
	check(ok, label);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_player_push_decay_golden_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	struct server_chunk* sc = NULL;
	for(int cx = -1; cx <= 1; cx++)
		for(int cz = -1; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++) {
			server_world_set_block_silent(&s.worlds[0], x, YF, z,
				(struct block_data) {.type = x >= 12 ? BLOCK_ICE : BLOCK_STONE});
			for(int y = YF + 1; y <= YF + 4; y++)
				server_world_set_block_silent(&s.worlds[0], x, y, z,
					(struct block_data) {.type = BLOCK_AIR});
		}
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = 20;
	s.player.dimension = WORLD_DIM_OVERWORLD;

	const float ground = 0.6F * 0.91F;  /* stone: slipperiness * 0.91F */
	const float ice = 0.98F * 0.91F;
	const float air = 0.91F;

	/* (A) A cart-sized shove (+d/4 <= 0.0125) below the old 0.01 gate: Beta
	 * moves the player and decays it by the ground friction. */
	run_case("(A) small shove on stone: applied, x0.546 per tick", 4.5, 4.5,
			 true, 0.008F, -0.004F, ground, 8);
	/* (B) A knockback-sized push on stone. */
	run_case("(B) knockback on stone: x0.546 per tick", 4.5, 8.5, true, 0.4F,
			 0.1F, ground, 8);
	/* (C) In the air: 0.91F. */
	run_case("(C) airborne: x0.91 per tick", 6.5, 4.5, false, 0.2F, 0.0F,
			 air, 8);
	/* (D) On ice (x >= 12): 0.98F * 0.91F. */
	run_case("(D) on ice: x(0.98*0.91) per tick", 13.5, 4.5, true, 0.05F,
			 0.05F, ice, 8);

	/* (F) R08 F2: Beta samples the friction block BEFORE moveEntity
	 * (EntityLiving.java:450-457), moves (:480), then decays by that saved
	 * factor (:486-487). Crossing stone -> ice keeps STONE friction for the
	 * crossing tick, and ice -> stone keeps ICE. One tick each way. */
	{
		place_player(11.99, 6.5, true, 0.05F, 0.0F);
		server_local_tick_actor_knockback(&s);
		check(s.player.vel_x == (double)0.05F * (double)ground,
			  "(F1) stone -> ice crossing tick decays by the PRE-move block (stone)");
		place_player(12.01, 6.5, true, -0.05F, 0.0F);
		server_local_tick_actor_knockback(&s);
		check(s.player.vel_x == (double)-0.05F * (double)ice,
			  "(F2) ice -> stone crossing tick decays by the PRE-move block (ice)");
	}

	/* (E) The tail (re-derived, G8-BLAST-VELOCITY v3/v4): Beta has NO
	 * small-motion cutoff (ASTRA-V2-REVIEW removed the old 1e-4 stop), and in
	 * IEEE double `0.546 * Double.MIN_VALUE` rounds back to MIN_VALUE, so
	 * Beta's own motion never reaches exactly 0 either. Every tick must equal
	 * the exact double recurrence; the server never moves the body; the
	 * tail is far below Packet28's 1/8000 quantum (it sends 0). */
	{
		place_player(4.5, 12.5, true, 0.3F, 0.0F);
		double x0 = s.player.x;
		double v = (double)0.3F;
		bool exact = true;
		for(int t = 0; t < 2000 && exact; t++) {
			server_local_tick_actor_knockback(&s);
			v *= (double)ground;
			exact = s.player.vel_x == v && s.player.vel_z == 0.0;
		}
		check(exact && s.player.x == x0 && fabs(s.player.vel_x) < 1e-300
			  && (int32_t)(s.player.vel_x * 8000.0) == 0,
			  "(E) 2000 ticks of the exact double recurrence, no cutoff; the "
			  "server never moves the body (G8); the tail quantizes to 0");
	}

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	printf("%s test_player_push_decay_golden (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
