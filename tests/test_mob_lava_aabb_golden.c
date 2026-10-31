/* next-gaps-0924/MOBLAVA -- every mob's lava contact is Beta's BOUNDING-BOX
 * scan, on the box Beta actually carries.
 *
 * Ground truth [mcb173-retromcp, net/minecraft/src]:
 *   Entity.java:126-133  setPosition: boundingBox.minY = posY - yOffset + ySize,
 *                        maxY = minY + height, minX/maxX = posX -/+ width/2.
 *   Entity.java:218-219  onEntityUpdate: if(handleLavaMovement()) setOnFireFromLava()
 *   Entity.java:234-238  setOnFireFromLava: !isImmuneToFire -> attackEntityFrom(null,4);
 *                        fire = 600
 *   Entity.java:566-568  handleLavaMovement: worldObj.isMaterialInBB(
 *                        boundingBox.expand(-0.1F, -0.4F, -0.1F), Material.lava)
 *   AxisAlignedBB.java:90-98  expand(dx,dy,dz): min - d, max + d (negative d
 *                        contracts: minX+0.1, minY+0.4, maxX-0.1, maxY-0.4)
 *   World.java:1458-1478 isMaterialInBB: cells [floor(min), floor(max + 1.0D))
 *                        on each axis; any lava-material block -> true. No
 *                        chunk preflight (an unloaded cell is id 0 = null block).
 *   Entity.java:480-490  moveEntity's burning box: isBoundingBoxBurning(
 *                        boundingBox.func_28195_e(0.001,0.001,0.001)) ->
 *                        dealFireDamage(1); ++fire (0 -> 300); else fire<=0 ->
 *                        fire = -fireResistance (-1, Entity.java:52).
 *   EntityLiving.java:194 yOffset = 0 for every living mob.
 *   EntityLiving.java:467, :628, :721 -- the lava MOVEMENT branch, the lava
 *                        jump and the 0.8 jump roll all call the SAME
 *                        handleLavaMovement() at the same pre-move position.
 *
 * The lateral rule (side cell yes, foot cell no; foot cell yes, contracted box
 * no) is pinned for the creeper in tests/test_lava_aabb_golden.c; this file
 * pins it on the two other body shapes (spider 1.4x0.9, big slime 2.4x2.4) and
 * then the VERTICAL rule the earlier port missed: Beta scans the box, whose
 * minY is posY + ySize. This engine keeps pos primary and the collision box at
 * pos + y_size (entity_monster.c ySize EASE/BANK, entity.c:102), so a mob that
 * has just stepped or hopped up (y_size > 0) must be scanned from its box, one
 * release-tail above its observable pos -- exactly as the water scan already
 * does (entity_handle_water_movement(pos - y_offset + y_size)).
 *
 * Hand-computed expectations (all doubles, Beta's arithmetic):
 *   S1 spider (8.5, 70.0, 8.5), lava (9,70,8): x cells floor(8.5-0.7+0.1)=7 ..
 *      floor(8.5+0.7-0.1+1)=10 excl -> 7,8,9; y floor(70.4)=70 ..
 *      floor(70.0+0.9-0.4+1)=71 excl -> 70. Cell (9,70,8) lava -> 4 + 600;
 *      the burning box (x 7.801..9.199 -> 7..9) then absorbs its 1 in the hurt
 *      window and ++fire -> 601. Foot cell (8,70,8) is air.
 *   S0 control spider at x 8.3: its maxX sits EXACTLY on the lava face
 *      (8.3 + 0.7 = 9.0). Lava scan x floor(8.3+0.7-0.1+1)=9 excl -> 7,8;
 *      burning box floor(9.0-0.001+1)=9 excl -> untouched: 20 / -1. This is
 *      the case that pins the burning box's 0.001 contraction (without it
 *      floor(9.0+1)=10 reaches the lava: 19 / 1).
 *   S2 spider (8.5, 70.70, 8.5) over lava (8,70,8): minY 71.1, maxY
 *      70.7+0.9-0.4=71.2 -> cell 71 only -> no lava 4; the burning box
 *      (minY+0.001 -> cell 70) lands its 1 and ++fire 0 -> 1: 19 / 1.
 *   B1 big slime (8.5, 70.0, 8.5), lava (8,72,8): y floor(70.4)=70 ..
 *      floor(70+2.4-0.4+1)=73 excl -> 70,71,72 -> 16-4 = 12, fire 600 -> 601
 *      (burning box y 70..floor(72.399+1)=73 excl includes 72). Foot cell air.
 *   Y1 creeper pos 70.5 with y_size 0.5 (box minY 71.0; mob motion units S2
 *      re-premise from the retired 1.5 engine jump bank -- see
 *      spawn_stepped_creeper), x 9.0 (box x
 *      8.7..9.3 -> lava scan cells floor(8.8)=8 .. floor(9.2+1)=10 excl ->
 *      8,9), standing on stone (9,70,8), lava (8,70,8). The hazards run
 *      before moveEntity's `ySize *= 0.4F`, on pos 70.5 / box minY
 *      71.0. Beta: minY+0.4 = 71.4 -> cells 71..floor(71.0+1.8-0.4+1)=73 excl
 *      -> 71,72: NO lava -> health 20; burning box floor(71.001)=71 -> not
 *      burning, fire<=0 -> -1. A pos-based scan reads floor(70.8)=70 -> the
 *      lava cell -> 16 / 600 (and a pos-based burning box floor(70.501)=70 ->
 *      300): that is the RED this file was authored against.
 *   Y0 control: Y1 geometry with stone instead of lava at (8,70,8): 20 / -1.
 *   Y2 the reverse: Y1 geometry, lava at (8,72,8) instead. Beta box cells
 *      71,72 -> lava -> 16 / 600, burning box 71..floor(72.799+1)=73 excl
 *      -> 72 -> 601. A pos-based scan (cells 70,71) misses it: 20, and its
 *      pos-based burning box (70..72) then ++fire 0 -> 1 with the 1 landing
 *      fresh: 19 / 1.
 */
#include <assert.h>
#include <math.h>
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
#include "support/mob_motion_fixture.h"
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
	assert(server_world_get_block(&s.worlds[0], x, y, z, &current));
	assert(current.type == expected.type);
}

static void clear_air(int x0, int x1, int y0, int y1, int z0, int z1) {
	for(int x = x0; x <= x1; x++)
		for(int y = y0; y <= y1; y++)
			for(int z = z0; z <= z1; z++)
				put(x, y, z, BLOCK_AIR);
}

/* Stone floor under the arena, air above it: every case starts from the same
 * static cells and puts back what it changed. */
static void arena(void) {
	clear_air(6, 11, 70, 75, 6, 11);
	for(int x = 6; x <= 11; x++)
		for(int z = 6; z <= 11; z++)
			put(x, 69, z, BLOCK_STONE);
}

/* Scenario prerequisites (ASTRA-SEED-POLICY-0925): the fixture asserts the
 * contact geometry it relies on, before and after tick 1, instead of letting a
 * seed choose it. Beta spawns mobs with zero motion (Entity/EntityLiving
 * constructors set no motionX/Y/Z); CaveWii's spawn helper adds a random
 * 0.05..0.25 velocity, so spawn() below zeroes it. */
static void prereq(const char* what, bool ok, float value) {
	if(ok)
		printf("PASS prerequisite %s (%.4f)\n", what, value);
	else {
		fprintf(stderr, "PREREQ-FAIL %s (%.4f)\n", what, value);
		failures++;
	}
}

static struct entity* spawn(int monster_id, float x, float feet_y, float z) {
	vec3 pos = {x, feet_y, z};
	struct entity* mob = server_local_spawn_monster_at(pos, monster_id, &s);
	assert(mob && mob->tick_server);
	mob->pos[0] = x;
	mob->pos[1] = feet_y;
	mob->pos[2] = z;
	mob->vel[0] = mob->vel[1] = mob->vel[2] = 0.0F;
	mob->y_size = 0.0F;
	test_mob_sync(mob); /* motion units S2 */
	mob->on_ground = true;
	mob->health = monster_def_get(monster_id)->health;
	mob->data.monster.fire_time = 0;
	return mob;
}

static void remove_mob(struct entity* mob) {
	uint32_t id = mob->id;
	free(mob);
	dict_entity_erase(s.entities, id);
}

static void case_spider_side_cell(void) {
	arena();
	/* S0: same body with maxX EXACTLY on the lava face -- no contact. The
	 * control must not move on tick 1, or the face-exact geometry is lost:
	 * Beta's EntityCreature.updatePlayerActionState may start a wander path
	 * on the very first tick (rand.nextInt(80) x2) and step the spider
	 * +0.03..0.04 x into the lava's reach (G1 review F1). The fixture takes
	 * the path away instead of choosing a seed: the spider (pos z 8.0 so its
	 * 2x2 path footprint is exactly cells x 7..8, z 7..8) is boxed in by
	 * 3-high stone on every side but the lava cell, so the path finder has no
	 * node to go to (lava is not a path node) and no wander path exists. The
	 * stone at (9,70,7) only touches the body's +X face; the burning box
	 * (contraction included) never scans stone as fire. */
	put(9, 70, 8, BLOCK_LAVA_STILL);
	for(int y = 70; y <= 72; y++) {
		for(int x = 6; x <= 9; x++) {
			put(x, y, 6, BLOCK_STONE);
			put(x, y, 9, BLOCK_STONE);
		}
		put(6, y, 7, BLOCK_STONE);
		put(6, y, 8, BLOCK_STONE);
		put(9, y, 7, BLOCK_STONE);
		if(y > 70)
			put(9, y, 8, BLOCK_STONE); /* the lava cell stays open at y 70 */
	}
	struct entity* control = spawn(MONSTER_SPIDER, 8.3F, 70.0F, 8.0F);
	struct AABB box;
	entity_make_world_bbox(control, &box);
	prereq("S0 pre-tick: at rest, maxX exactly on the lava face x=9",
		box.x2 == 9.0F && control->vel[0] == 0.0F, box.x2);
	assert(!control->tick_server(control, &s));
	entity_make_world_bbox(control, &box);
	prereq("S0 after tick 1: did not move, maxX still exactly on the face",
		control->pos[0] == 8.3F && control->pos[2] == 8.0F && box.x2 == 9.0F,
		box.x2);
	golden("S0 spider control (lava out of reach) health", control->health,
		20);
	golden("S0 spider control (lava out of reach) fire",
		control->data.monster.fire_time, -1);
	remove_mob(control);
	for(int y = 70; y <= 72; y++) /* take the enclosure away again */
		for(int x = 6; x <= 9; x++)
			for(int z = 6; z <= 9; z++)
				if(!(x == 9 && y == 70 && z == 8))
					put(x, y, z, BLOCK_AIR);

	/* S1: the 1.4-wide body reaches the side cell x = 9; its foot cell is air. */
	struct entity* spider = spawn(MONSTER_SPIDER, 8.5F, 70.0F, 8.5F);
	assert(!spider->tick_server(spider, &s));
	golden("S1 spider side cell (not foot) health", spider->health, 16);
	golden("S1 spider side cell (not foot) fire",
		spider->data.monster.fire_time, 601);
	golden("S1 spider side cell (not foot) iframe",
		spider->data.monster.hurt_resist_time, 9);
	remove_mob(spider);
	put(9, 70, 8, BLOCK_AIR);
}

static void case_spider_foot_cell_contracted_away(void) {
	arena();
	put(8, 70, 8, BLOCK_LAVA_STILL);
	struct entity* spider = spawn(MONSTER_SPIDER, 8.5F, 70.70F, 8.5F);
	assert(!spider->tick_server(spider, &s));
	golden("S2 spider foot cell, box contracted away: health",
		spider->health, 19);
	golden("S2 spider foot cell, box contracted away: fire",
		spider->data.monster.fire_time, 1);
	remove_mob(spider);
	put(8, 70, 8, BLOCK_AIR);
}

static void case_big_slime_upper_cell(void) {
	arena();
	put(8, 72, 8, BLOCK_LAVA_STILL);
	struct entity* slime = spawn(MONSTER_SLIME_BIG, 8.5F, 70.0F, 8.5F);
	assert(!slime->tick_server(slime, &s));
	golden("B1 big slime upper cell (not foot) health", slime->health, 12);
	golden("B1 big slime upper cell (not foot) fire",
		slime->data.monster.fire_time, 601);
	remove_mob(slime);
	put(8, 72, 8, BLOCK_AIR);
}

/* Y-cases: the creeper's box straddles x cells 8 and 9, rests on stone at
 * (9,70,8) with box minY 71.0 and carries a banked step: pos 70.5 + y_size
 * 0.5 (Entity.setPosition: minY = posY - yOffset + ySize). Mob motion units
 * S2 re-premise: Beta's ySize is only ever the stepHeight bank (frac(minY) +
 * 0.01, <= ~0.51); the old 1.5 was the retired engine jump bank, and at
 * posY 69.5 Beta's pre-move suffocation corners (posY + eye +- 0.05) would
 * reach the stone -- so the scene now uses a Beta-reachable 0.5, which keeps
 * the same discrimination (the pos-based scan sees the lava cell at y 70, the
 * box-based one does not; the box top reaches y 72, the pos top does not). */
static struct entity* spawn_stepped_creeper(void) {
	struct entity* mob = spawn(MONSTER_CREEPER, 9.0F, 70.5F, 8.5F);
	mob->y_size = 0.5F;
	test_mob_sync(mob); /* motion units S2 */
	return mob;
}

static void pin_stepped(struct entity* mob) {
	mob->pos[0] = 9.0F;
	mob->pos[1] = 70.5F;
	mob->pos[2] = 8.5F;
	mob->y_size = 0.5F;
	mob->vel[0] = mob->vel[1] = mob->vel[2] = 0.0F;
	test_mob_sync(mob); /* motion units S2 */
	mob->on_ground = true;
}

static void case_ysize_control(void) {
	arena();
	put(8, 70, 8, BLOCK_STONE);
	put(9, 70, 8, BLOCK_STONE);
	struct entity* mob = spawn_stepped_creeper();
	assert(!mob->tick_server(mob, &s));
	golden("Y0 stepped creeper control health", mob->health, 20);
	golden("Y0 stepped creeper control fire", mob->data.monster.fire_time,
		-1);
	/* Entity.moveEntity: `ySize *= 0.4F` -> 0.2 (the box minY stays 71.0,
	 * posY eases to 71.0 - 0.2). */
	golden("Y0 stepped creeper release: y_size 0.2",
		(long)lroundf(mob->y_size * 100.0F), 20);
	remove_mob(mob);
	put(8, 70, 8, BLOCK_AIR);
	put(9, 70, 8, BLOCK_AIR);
}

static void case_ysize_lava_below_box(void) {
	arena();
	put(8, 70, 8, BLOCK_LAVA_STILL);
	put(9, 70, 8, BLOCK_STONE);
	struct entity* mob = spawn_stepped_creeper();
	for(int tick = 1; tick <= 3; tick++) {
		pin_stepped(mob);
		assert(!mob->tick_server(mob, &s));
		golden("Y1 stepped creeper, lava under the pos but below the box: health",
			mob->health, 20);
		golden("Y1 stepped creeper, lava under the pos but below the box: fire",
			mob->data.monster.fire_time, -1);
	}
	remove_mob(mob);
	put(8, 70, 8, BLOCK_AIR);
	put(9, 70, 8, BLOCK_AIR);
}

static void case_ysize_lava_at_box_top(void) {
	arena();
	put(8, 70, 8, BLOCK_STONE);
	put(9, 70, 8, BLOCK_STONE);
	put(8, 72, 8, BLOCK_LAVA_STILL);
	struct entity* mob = spawn_stepped_creeper();
	assert(!mob->tick_server(mob, &s));
	golden("Y2 stepped creeper, lava at the box top above the pos scan: health",
		mob->health, 16);
	golden("Y2 stepped creeper, lava at the box top above the pos scan: fire",
		mob->data.monster.fire_time, 601);
	remove_mob(mob);
	put(8, 70, 8, BLOCK_AIR);
	put(9, 70, 8, BLOCK_AIR);
	put(8, 72, 8, BLOCK_AIR);
}

int main(void) {
	test_fixture_dir("test_mob_lava_aabb_golden_tmp", fixture, sizeof fixture);
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
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	/* Pin the tick-1 wander rolls. With rand_gen_seed's clock seed, the S0
	 * control spider can step into the side lava cell before the burning-box
	 * scan, giving 19/1 instead of the intended out-of-reach 20/-1. */
	s.rand_src.seed = rand_test_scenario_seed(0x9E3779B9u); /* pinned; CW_TEST_SEED_FORCE replays */
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &chunk) && chunk);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
		&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.weather.rain_strength = s.weather.prev_rain_strength = 0.0F;

	case_spider_side_cell();
	case_spider_foot_cell_contracted_away();
	case_big_slime_upper_cell();
	case_ysize_control();
	case_ysize_lava_below_box();
	case_ysize_lava_at_box_top();

	draining = 0;
	thread_join(&worker);
	string_clear(level);
	test_fixture_dir_cleanup(fixture);
	if(failures != 0) {
		fprintf(stderr, "test_mob_lava_aabb_golden: FAIL (%d assertions)\n",
			failures);
		return 1;
	}
	puts("test_mob_lava_aabb_golden: PASS");
	return 0;
}
