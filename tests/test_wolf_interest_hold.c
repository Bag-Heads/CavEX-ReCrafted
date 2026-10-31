/* Beta 1.7.3 wolf head tilt: looksWithInterest keeps the look latch alive.
 *
 * [mcb173-retromcp src_original, EntityWolf.java]
 *  - onLivingUpdate :115-131: looksWithInterest = false; then, if
 *    hasCurrentTarget() && !hasPath() && !isWolfAngry() and the target is an
 *    EntityPlayer holding a bone (untamed) or the wolf's favorite meat
 *    (tamed), looksWithInterest = true.
 *  - onUpdate :142-153: field_25054_c = field_25048_b (headRotationCourseOld =
 *    headRotationCourse); headRotationCourse += (target - course) * 0.4F,
 *    target 1 when interested, 0 otherwise; and
 *        if(this.looksWithInterest) this.numTicksToChaseTarget = 10;
 *  - getInterestedAngle :207-209: lerp(old, course, partial) * 0.15F * PI,
 *    the head/ears/snout Z roll [ModelWolf.setRotationAngles :111-115].
 * [EntityLiving.updatePlayerActionState :689-711]: the look latch
 * (currentTarget) drops when numTicksToChaseTarget-- <= 0 or the player is
 * beyond 8 blocks. With the onUpdate pin the hold is 10 at every latch check
 * (decremented to 9), so a wolf keeps looking -- and keeps its head tilted --
 * for as long as the bone is shown within range.
 *
 * Fable C20 F3(a): the pin was not ported, so the port dropped the latch
 * 10-29 ticks after it formed and the tilt flickered off until the next 2%
 * re-roll. This drives the REAL wolf server tick (tick_server) and checks:
 *  I1 while a bone is shown within range, the wolf stays interested for 120
 *     ticks (6 s) and its head course converges to and stays at 1;
 *  I2 the latch hold is pinned (never below 9 after an interested tick);
 *  I3 once the bone is put away the interest clears at once, the head course
 *     decays by the 0.4 rule, and the latch lapses within its 10-tick hold.
 * Non-aborting: ok/FAIL lines. */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
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
#include "support/mob_motion_fixture.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/combat.h" // PLAYER_EYE_HEIGHT
#include "item/inventory.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_interface.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "wolf_test_common.h"

extern struct inventory_logic inventory_logic_player;

static unsigned failures;
static void check(bool cond, const char* label) {
	printf("%s %s\n", cond ? "ok  " : "FAIL", label);
	if(!cond)
		failures++;
}

static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set_block(struct server_local* s, int x, int y, int z, int type) {
	/* Writing a cell's current id is a no-op that reports false (Beta's
	 * setBlockAndMetadataWithNotify), so check the RESULT, not the call. */
	(void)server_world_set_block(&s->worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type, .metadata = 0});
	struct block_data got;
	assert(server_world_get_block(&s->worlds[0], x, y, z, &got)
		   && got.type == type);
}

static void hold_item(struct server_local* s, int id) {
	struct item_data item = {.id = (uint16_t)id, .durability = 0,
							 .count = id ? 1 : 0};
	inventory_set_slot(&s->player.inventory, INVENTORY_SLOT_HOTBAR, item);
	inventory_set_hotbar(&s->player.inventory, 0);
}

int main(void) {
	char dir[128];
	test_fixture_dir("test_wolf_interest_hold_tmp", dir, sizeof dir);
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	MKDIR(dir);
	MKDIR(test_fixture_join(dir, "/region"));
	remove(test_fixture_join(dir, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, dir);

	static struct server_local s;
	/* Overworld-only fixture: worlds[0] named explicitly (world alias gate). */
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 5150, true);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 20260927u; /* deterministic wander/latch draws */
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	/* The wolf in a closed 1x1 stone cell (so a wander roll finds no path and
	 * !hasPath() holds -- the gate under test is the latch hold, not
	 * wandering); the player 3 blocks away, eye level, holding a bone. */
	const int WX = 10, WZ = 10, Y = 70;
	for(int dx = -1; dx <= 1; dx++)
		for(int dz = -1; dz <= 1; dz++) {
			set_block(&s, WX + dx, Y - 1, WZ + dz, BLOCK_STONE);
			set_block(&s, WX + dx, Y + 2, WZ + dz, BLOCK_STONE);
			for(int y = Y; y <= Y + 1; y++)
				set_block(&s, WX + dx, y, WZ + dz,
						  (dx || dz) ? BLOCK_GLASS : BLOCK_AIR);
		}
	struct entity* wolf = wolf_test_make(&s, 1, MONSTER_WOLF);
	wolf->pos[0] = wolf->pos_old[0] = WX + 0.5F;
	wolf->pos[1] = wolf->pos_old[1] = (float)Y;
	wolf->pos[2] = wolf->pos_old[2] = WZ + 0.5F;
	test_mob_sync(wolf); /* motion units S2: the fixture pins the doubles */
	wolf->on_ground = true;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.x = WX + 3.5;
	s.player.y = Y + PLAYER_EYE_HEIGHT;
	s.player.z = WZ + 0.5;
	hold_item(&s, ITEM_BONE);

	/* The 2% roll has just hit: the look latch forms with its shortest Beta
	 * hold (10 + nextInt(20) = 10). */
	wolf->data.monster.wolf_has_current_target = true;
	wolf->data.monster.wolf_current_target_hold = 10;

	unsigned interested = 0, held_ok = 0, pathed = 0;
	float course = 0.0F;
	bool converged = true;
	for(int tick = 0; tick < 120; tick++) {
		assert(!wolf->tick_server(wolf, &s));
		if(wolf->data.monster.path.length > 0
		   && !path_entity_is_finished(&wolf->data.monster.path))
			pathed++;
		if(wolf->data.monster.wolf_interested)
			interested++;
		if(!wolf->data.monster.wolf_interested
		   || wolf->data.monster.wolf_current_target_hold >= 9)
			held_ok++;
		course = wolf->data.monster.wolf_head_rotation;
		if(tick >= 20 && course < 0.999F)
			converged = false;
	}
	printf("interested %u/120, hold ok %u/120, path ticks %u, course %.4f\n",
		   interested, held_ok, pathed, (double)course);
	check(pathed == 0, "setup: the caged wolf never paths (hasPath false)");
	check(interested == 120,
		  "I1 a bone shown within range keeps the wolf interested (no flicker)");
	check(converged, "I1 the head course converges to 1 and stays there");
	check(held_ok == 120, "I2 the latch hold is pinned while interested");

	/* Put the bone away. */
	hold_item(&s, 0);
	float before = wolf->data.monster.wolf_head_rotation;
	assert(!wolf->tick_server(wolf, &s));
	check(!wolf->data.monster.wolf_interested,
		  "I3 no bone: the interest clears on the next tick");
	check(fabsf(wolf->data.monster.wolf_head_rotation - before * 0.6F) < 1e-5F
			  && wolf->data.monster.wolf_head_rotation_old == before,
		  "I3 the head course decays by Beta's 0.4 rule");
	unsigned lapsed_at = 0;
	for(unsigned tick = 1; tick <= 12 && !lapsed_at; tick++) {
		assert(!wolf->tick_server(wolf, &s));
		if(!wolf->data.monster.wolf_has_current_target)
			lapsed_at = tick;
	}
	check(lapsed_at != 0 && lapsed_at <= 10,
		  "I3 the look latch lapses within its 10-tick hold");

	wolf_test_clear(&s);
	g_drain_run = 0;
	thread_join(&drain);
	printf("%s test_wolf_interest_hold (%u failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
