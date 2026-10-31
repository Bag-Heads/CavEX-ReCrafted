/* next-gaps-0924/X2 -- the Giant's box is Beta's FLOAT PRODUCT, not the
 * decimal literal.
 *
 * Ground truth [740c583]: Entity.java:37-38 defaults `width = 0.6F;
 * height = 1.8F`; EntityGiantZombie.java:11 `setSize(width * 6.0F,
 * height * 6.0F)`. Evaluated in Java float arithmetic:
 *   0.6F * 6.0F = 3.60000014F  (bits 0x40666667; exact, no rounding)
 *   1.8F * 6.0F = 10.7999992F  (bits 0x412CCCCC; a tie, rounded to even)
 * The literals 3.6f / 10.8f are 3.59999990F (0x40666666) and 10.8000002F
 * (0x412CCCCD) -- one ulp off on each axis, the other way. The eye,
 * EntityLiving.getEyeHeight `height * 0.85F`, is 9.17999935F (0x4112E147),
 * one ulp under the 9.18f literal (0x4112E148).
 *
 * One ulp is observable wherever a box edge lands within an ulp of a cell
 * boundary. The positions below were found analytically (exhaustive float
 * search over the neighbourhood, then checked in Beta's double arithmetic):
 *
 * (a) Collision AABB (entity_make_world_bbox, the same def-driven box as the
 *     MOB_BBOX_THUNKS mover box):
 *   x: posX = 0.2F - 2 ulp = 0.199999973F (bits 0x3E4CCCCB).
 *      maxX = posX + w/2:  literal 1.79999995 -> 1.99999988F -> cell 1;
 *      Beta 1.80000007 -> 2.0F -> cell 2 (Beta double: 2.0000000447 -> 2).
 *   y: posY = 0.2F. maxY = posY + h: literal -> 11.0F -> cell 11;
 *      Beta -> 10.9999990F -> cell 10 (Beta double: 10.99999924 -> 10).
 *
 * (b) server_world_aabb_has_lava's contracted box, driven through the real
 *     monster tick (boundingBox.expand(-0.1F, -0.4F, -0.1F) ->
 *     World.isMaterialInBB, which scans [floor(min), floor(max + 1))):
 *   X: posX = 0.3F. floor(posX + w/2 - 0.1F + 1): literal 2.99999996 -> 2
 *      (x = 2 NOT scanned); Beta 3.00000008 -> 3 (x = 2 scanned). Lava at
 *      x = 2 burns the Beta giant, not the literal one.
 *   Y: posY = 4.6F. floor(posY + h - 0.4F + 1): literal 16.00000009 -> 16
 *      (y = 15 scanned); Beta 15.99999914 -> 15 (y = 15 NOT scanned). Lava at
 *      y = 15 burns the literal giant, not the Beta one.
 *   Each lava case has a control (no lava / lava one cell inside) so a stray
 *   natural block or a dead scan cannot fake the verdict.
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
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
#include "block/aabb.h"
#include "block/blocks.h"
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/monsters_object.h"
#include "item/inventory.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static struct server_local s;
static char fixture[128];
static volatile int draining = 1;
static int failures;

static void golden(const char* case_name, long actual, long expected) {
	if(actual != expected) {
		fprintf(stderr, "RED %s: actual=%ld expected=%ld\n", case_name,
			actual, expected);
		failures++;
	} else {
		printf("PASS %s: %ld\n", case_name, actual);
	}
}

static void golden_bits(const char* case_name, float actual,
	uint32_t expected) {
	uint32_t got;
	memcpy(&got, &actual, sizeof got);
	if(got != expected) {
		fprintf(stderr, "RED %s: actual=0x%08lX (%.9g) expected=0x%08lX\n",
			case_name, (unsigned long)got, (double)actual,
			(unsigned long)expected);
		failures++;
	} else {
		printf("PASS %s: 0x%08lX (%.9g)\n", case_name, (unsigned long)got,
			(double)actual);
	}
}

static float float_from_bits(uint32_t bits) {
	float f;
	memcpy(&f, &bits, sizeof f);
	return f;
}

static void* drain(void* unused) {
	(void)unused;
	while(draining) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void put(int x, int y, int z, int block) {
	struct block_data expected = {.type = (uint8_t)block};
	if(server_world_set_block(&s.worlds[0], x, y, z, expected))
		return;
	struct block_data current;
	assert(server_world_get_block(&s.world, x, y, z, &current));
	assert(current.type == expected.type);
}

static void clear_air(int x0, int x1, int y0, int y1, int z0, int z1) {
	for(int x = x0; x <= x1; x++)
		for(int y = y0; y <= y1; y++)
			for(int z = z0; z <= z1; z++)
				put(x, y, z, BLOCK_AIR);
}

/* Scenario prerequisites (ASTRA-SEED-POLICY-0925): the fixture asserts the
 * contact geometry it relies on, before and after tick 1, instead of letting a
 * seed choose it. Beta spawns mobs with zero motion (Entity/EntityLiving
 * constructors set no motionX/Y/Z); CaveWii's spawn helper adds a random
 * 0.05..0.25 velocity, so the spawn helpers below zero it. */
static void prereq(const char* what, bool ok, float value) {
	if(ok)
		printf("PASS prerequisite %s (%.4f)\n", what, value);
	else {
		fprintf(stderr, "PREREQ-FAIL %s (%.4f)\n", what, value);
		failures++;
	}
}

static struct entity* spawn_giant(float x, float feet_y, float z) {
	vec3 pos = {x, feet_y, z};
	struct entity* mob = server_local_spawn_monster_at(pos, MONSTER_GIANT, &s);
	assert(mob && mob->tick_server);
	mob->pos[0] = x;
	mob->pos[1] = feet_y;
	mob->pos[2] = z;
	mob->vel[0] = mob->vel[1] = mob->vel[2] = 0.0F;
	mob->health = 200;
	mob->data.monster.fire_time = 0;
	return mob;
}

/* Beta's setSize arguments, evaluated at runtime in float (volatile defeats
 * any wider constant folding), and pinned to their literal bits. */
static void case_def_bits(void) {
	volatile float entity_w = 0.6F, entity_h = 1.8F, six = 6.0F;
	const float beta_w = entity_w * six;
	const float beta_h = entity_h * six;
	golden_bits("0 Beta 0.6F*6.0F", beta_w, UINT32_C(0x40666667));
	golden_bits("0 Beta 1.8F*6.0F", beta_h, UINT32_C(0x412CCCCC));
	const struct monster_def* d = monster_def_get(MONSTER_GIANT);
	golden_bits("1 giant box_w", d->box_w, UINT32_C(0x40666667));
	golden_bits("1 giant box_h", d->box_h, UINT32_C(0x412CCCCC));
	/* EntityLiving.getEyeHeight: height * 0.85F on the product above
	 * = 9.17999935F; the old 9.18f literal is 0x4112E148. */
	volatile float eye_scale = 0.85F;
	golden_bits("0 Beta (1.8F*6.0F)*0.85F", beta_h * eye_scale,
		UINT32_C(0x4112E147));
	golden_bits("1 giant eye_height", d->eye_height, UINT32_C(0x4112E147));
}

static void case_collision_box(void) {
	struct entity* giant = spawn_giant(8.5F, 70.0F, 8.5F);
	golden_bits("2 entity width", giant->width, UINT32_C(0x40666667));
	golden_bits("2 entity height", giant->height, UINT32_C(0x412CCCCC));

	struct AABB box;
	giant->pos[0] = float_from_bits(UINT32_C(0x3E4CCCCB)); /* 0.2F - 2 ulp */
	giant->pos[1] = 70.0F;
	giant->pos[2] = 8.5F;
	entity_make_world_bbox(giant, &box);
	golden("3 collision maxX cell at posX 0x3E4CCCCB",
		(long)floorf(box.x2), 2);

	giant->pos[0] = 8.5F;
	giant->pos[1] = 0.2F;
	entity_make_world_bbox(giant, &box);
	golden("4 collision maxY cell at posY 0.2F", (long)floorf(box.y2), 10);
}

/* Width: lava at x = 2 is inside Beta's contracted scan, outside the
 * literal's. Control first: the same body with no lava stays unburnt.
 * Fire values follow the accepted lava-burn-0924 (G6) semantics: an unlit
 * mob's first air tick rests at -fireResistance = -1 (Entity.java:489-490,
 * :52); a lava hit sets 600 (:218-219) and moveEntity's burning box then
 * adds one (:484-486) -> 601 (test_lava_aabb_golden A). */
static void case_lava_width(void) {
	clear_air(0, 3, 70, 81, 6, 10);
	put(1, 69, 8, BLOCK_STONE);

	struct entity* control = spawn_giant(0.3F, 70.0F, 8.5F);
	assert(!control->tick_server(control, &s));
	golden("5 width control (no lava) health", control->health, 200);
	golden("5 width control (no lava) fire", control->data.monster.fire_time,
		-1);

	put(2, 71, 8, BLOCK_LAVA_STILL);
	struct entity* giant = spawn_giant(0.3F, 70.0F, 8.5F);
	assert(!giant->tick_server(giant, &s));
	golden("6 width lava x=2 health", giant->health, 196);
	/* 600, not 601, and that IS Beta for this fixture: only chunk (0,0) is
	 * resident, and the giant's burning box (contract 0.001: x -1.499..2.099,
	 * floor(minX) = -2) reaches into chunk x = -1. World.isBoundingBoxBurning
	 * scans only after checkChunksExist(minX, minY, minZ, floor(maxX + 1), ..)
	 * (World.java:1395-1402), which is false for the missing chunk, so the
	 * scan never runs: no dealFireDamage(1), no ++fire, and the else arm
	 * (Entity.java:489) leaves a positive fire alone. handleLavaMovement's
	 * isMaterialInBB has no such preflight, hence the lava 4 + 600
	 * (Entity.java:218-219, 234-237). Row 6b loads the neighbour chunk. */
	golden("6 width lava x=2 fire", giant->data.monster.fire_time, 600);

	/* 6b: the same body with chunk (-1, 0) resident -- the preflight passes
	 * and the scan finds the side cell x = 2 inside floor(maxX + 1) = 3:
	 * 600 + 1 = 601 (Entity.java:481-486), health still 196 (the 1 lands in
	 * the 4's hurt window). */
	struct server_chunk* west = NULL;
	assert(server_world_load_chunk(&s.world, -1, 0, &west) && west);
	clear_air(-2, -1, 69, 81, 6, 10);
	struct entity* both = spawn_giant(0.3F, 70.0F, 8.5F);
	struct AABB box;
	entity_make_world_bbox(both, &box);
	prereq("6b pre-tick: at rest, body reaches the lava side cell x=2 (maxX)",
		both->vel[0] == 0.0F && both->vel[1] == 0.0F && both->vel[2] == 0.0F
			&& box.x2 > 2.0F,
		box.x2);
	assert(!both->tick_server(both, &s));
	entity_make_world_bbox(both, &box);
	prereq("6b after tick 1: body still reaches x=2 (maxX)", box.x2 > 2.0F,
		box.x2);
	golden("6b width lava x=2, west chunk resident, health", both->health,
		196);
	golden("6b width lava x=2, west chunk resident, fire",
		both->data.monster.fire_time, 601);
	put(2, 71, 8, BLOCK_AIR);
}

/* Height: lava at y = 15 is inside the literal's contracted scan, outside
 * Beta's handleLavaMovement box (contract 0.1/0.4/0.1: no 4-point hit, no
 * 600). Control: lava at y = 14 (inside both) burns, proving the scan is
 * live at this position. Beta's isBoundingBoxBurning box (contract 0.001,
 * Entity.java:484) DOES reach y = 15 (top 4.6 + 10.8 - 0.001), and lava is a
 * burning material there: dealFireDamage(1) -> health 199, and ++fire from
 * a fresh entity's 0 -> 1 (:486-487; the == 0 -> 300 arm needs the idle -1
 * of a previous air tick). */
static void case_lava_height(void) {
	clear_air(6, 10, 4, 16, 6, 10);

	put(8, 14, 8, BLOCK_LAVA_STILL);
	struct entity* control = spawn_giant(8.5F, 4.6F, 8.5F);
	assert(!control->tick_server(control, &s));
	golden("7 height control (lava y=14) health", control->health, 196);
	golden("7 height control (lava y=14) fire",
		control->data.monster.fire_time, 601);
	put(8, 14, 8, BLOCK_AIR);

	put(8, 15, 8, BLOCK_LAVA_STILL);
	struct entity* giant = spawn_giant(8.5F, 4.6F, 8.5F);
	assert(!giant->tick_server(giant, &s));
	golden("8 height lava y=15 health", giant->health, 199);
	golden("8 height lava y=15 fire", giant->data.monster.fire_time, 1);
	put(8, 15, 8, BLOCK_AIR);
}

int main(void) {
	test_fixture_dir("test_giant_box_golden_tmp", fixture, sizeof fixture);
	assert(clin_init());
	assert(svin_init());
	struct thread worker;
	assert(thread_create(&worker, drain, NULL, 64));
	blocks_init();
	MKDIR(fixture);
	MKDIR(test_fixture_join(fixture, "/region"));
	string_t level;
	string_init_set_str(level, fixture);
	memset(&s, 0, sizeof s);
	dict_entity_init(s.entities);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	/* Pin the mob's spawn velocity and tick-1 wander: a time-based seed can
	 * move it off the lava side cell before the burning-box scan in case 6b. */
	s.rand_src.seed = rand_test_scenario_seed(0x51DEB002u); /* pinned; CW_TEST_SEED_FORCE replays */
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&s.world, 0, 0, &chunk) && chunk);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
		&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.weather.rain_strength = s.weather.prev_rain_strength = 0.0F;

	case_def_bits();
	case_collision_box();
	case_lava_width();
	case_lava_height();

	draining = 0;
	thread_join(&worker);
	string_clear(level);
	test_fixture_dir_cleanup(fixture);
	if(failures != 0) {
		fprintf(stderr, "test_giant_box_golden: FAIL (%d assertions)\n",
			failures);
		return 1;
	}
	puts("test_giant_box_golden: PASS");
	return 0;
}
