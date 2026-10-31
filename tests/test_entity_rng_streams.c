/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * test_entity_rng_streams -- RED gate for the per-entity gameplay RNG card
 * (Astra ruling tmp/jobs/split-dimensions-plan-0924/out/ASTRA-RNG-0925.md).
 *
 * Beta 1.7.3 gives EVERY entity its own generator: Entity.java:50
 * one java.util.Random per entity instance. Mob AI, item lava bounce and
 * projectile spread all draw from that per-instance stream; the world's
 * random ticks draw from World.rand. CaveWii today draws most entity rolls
 * from the one shared server stream s->rand_src (items from one shared
 * s->entity_rng), so one entity's draw count shifts the world's random-tick
 * picks and every other entity's rolls (GOLDEN-result.md, the tick-568
 * coupling). The ruling makes this parity debt, to be fixed by per-entity
 * streams after G1 + A1c.
 *
 * I02 (vehicle-smoke-1004 / board #8): imported from 626edc43 now that its
 * last open checks (d1/d2) pass; registered as an ordinary test (no
 * WILL_FAIL), plus cases (f) and (g): exact projectile spread goldens.
 *
 * STATUS (historical, 626edc43): RED BY DESIGN. Registered WILL_FAIL. Every RED() check below is a
 * behaviour the per-entity card must deliver; a failing RED() is recorded
 * and the process exits 1 after ALL cases ran (so one run lists every open
 * item). Setup / fixture preconditions use assert() and may abort -- NOTE:
 * under WILL_FAIL an abort also reads as "passed", so read the log for the
 * final "RED SUMMARY" line. When the card lands, every RED() passes, the
 * process exits 0, and WILL_FAIL must be dropped in the same commit.
 *
 * Streams are NOT claimed to be java.util.Random (CODESTYLE two-RNG rule;
 * rand_gen_* is xorshift32): the checks are ownership/independence only.
 *
 * Cases:
 *  (a) a mob's swim/wander tick leaves the world random-tick stream alone
 *      (reproduces the tick-568 coupling: a pig swimming in a pit shifts
 *      server_world_random_tick's picks). Beta: EntityCreature.java:47
 *      `rand.nextInt(100)`, :106 `rand.nextFloat() < 0.8F` (swim, drawn in
 *      the path branch whether wet or dry), EntityLiving.java:723 (idle swim)
 *      -- all the entity's own rand; World random ticks use World.rand.
 *  (b) two entities' draws are independent: extra draws by pig A do not
 *      change pig B's trajectory (Entity.java:50, one Random per instance).
 *  (c) item-entity lava motion comes from the item's OWN stream: EntityItem
 *      .java:46-51 draws four own-rand nextFloat() motion values; a second
 *      item's lava draws must not change the first item's motion.
 *      (ITEMENT-AUDIT-0925 "Motion RNG" row.)
 *  (d) dispensed projectile spread draws from the projectile's own stream:
 *      BlockDispenser.java:98-111 builds the arrow/snowball and
 *      EntityArrow.java:55 (setArrowHeading) draws three
 *      own-rand nextGaussian() values; dispenseItem's world Random is used
 *      ONLY by the generic-item branch (:118-123) and slot choice uses
 *      TileEntityDispenser.java:39 dispenserRandom. So dispensing an arrow
 *      or a snowball must not advance s->rand_src at all.
 *      (FIRE-AUDIT-0925 "Dispenser projectile RNG" row.)
 *  (e) seeding / lifetime: entity creation draws nothing from the world
 *      stream (Beta constructs mobs at rest; Entity.java:50 + EntityLiving's
 *      constructor use a fresh Random or Math.random, never World.rand --
 *      SEED-POLICY-result.md spawn-velocity finding: server_local_spawn_
 *      monster_at draws 4 rand_src floats); distinct entities get distinct
 *      streams; streams are reproducible for a fixed world seed + spawn
 *      order; no per-tick reseed (a per-entity sequence is not constant).
 *
 * NOT covered here (needs the card's API): the exact dry/wet path-branch
 * swim draw COUNT on the entity's own stream (ruling item 10/11,
 * MOBPHYS-pathfind-diag.md); add it when the per-entity stream is readable.
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
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
#include "block/blocks_data.h"
#include "block/dispenser_logic.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/monsters_object.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_dispenser.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "profile.h"
#include "util.h"
#include "entity/combat.h"
#include "worldgen/java_random.h"

#define FLOOR_Y 68
#define FEET_Y 69
#define SEED_WORLD_STREAM 0x5EED0A11u
#define SEED_AFTER_SPAWN 0x0B5E55EDu
#define SEED_ITEM_STREAM 0x17E3A5EDu

static struct server_local s;
static char s_dir[160];
static int s_run;

/* ---- RED bookkeeping ------------------------------------------------------ */
static int red_total, red_open;
static void red(const char* id, bool ok, const char* what) {
	red_total++;
	if(ok) {
		printf("  [%s] GREEN: %s\n", id, what);
	} else {
		red_open++;
		printf("  [%s] RED (open): %s\n", id, what);
	}
}

/* ---- fixture ---------------------------------------------------------------- */
static void* drain_entry(void* arg) {
	(void)arg;
	while(1) {
		clin_drain_discard();
		thread_msleep(1);
	}
	return NULL;
}

static void set(int x, int y, int z, int type) {
	assert(server_world_set_block(&s.worlds[0], x, y, z,
								  (struct block_data) {.type = (uint8_t)type}));
}

/* A fresh, identical world per run: every comparison below runs the SAME
 * setup twice (control vs. extra draws), so no state leaks between runs. */
static void fixture_open(void) {
	char base[64];
	snprintf(base, sizeof base, "test_entity_rng_streams_%d", s_run++);
	test_fixture_dir(base, s_dir, sizeof s_dir);
	MKDIR(s_dir);
	MKDIR(test_fixture_join(s_dir, "/region"));
	remove(test_fixture_join(s_dir, "/region/r.0.0.mcr"));

	memset(&s, 0, sizeof s);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	dict_entity_init(s.entities);
	string_t level;
	string_init_set_str(level, s_dir);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
						true /* superflat */);
	string_clear(level);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 128;
	s.rand_src.seed = SEED_WORLD_STREAM;
	s.entity_rng.seed = SEED_ITEM_STREAM;
	s.container_drop_rng.seed = 0xC0417A1Eu;
	s.dispenser_break_rng.seed = 0xD15BE4Eu;

	struct server_chunk* sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 0, 0, &sc) && sc);
	sc = NULL;
	assert(server_world_load_chunk(&s.worlds[0], 1, 0, &sc) && sc);
	for(int x = 0; x <= 30; x++)
		for(int z = 0; z <= 15; z++)
			set(x, FLOOR_Y, z, BLOCK_STONE);
}

static void fixture_close(void) {
	bool again = true;
	while(again) {
		again = false;
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		if(!dict_entity_end_p(it)) {
			uint32_t id = dict_entity_ref(it)->key;
			struct entity* e = dict_entity_ref(it)->value;
			dict_entity_erase(s.entities, id);
			free(e);
			again = true;
		}
	}
	dict_entity_clear(s.entities);
	server_world_discard(&s.worlds[0]);
	test_fixture_dir_cleanup(s_dir);
}

/* A 1x1 still-water pit in the floor, walled two high: a pig in it swims
 * every tick and can never climb out or reach the other entities. */
static void build_pit(int x, int z) {
	set(x, FLOOR_Y, z, BLOCK_WATER_STILL);
	set(x, FLOOR_Y - 1, z, BLOCK_STONE);
	for(int dx = -1; dx <= 1; dx++)
		for(int dz = -1; dz <= 1; dz++)
			if(dx || dz) {
				set(x + dx, FEET_Y, z + dz, BLOCK_STONE);
				set(x + dx, FEET_Y + 1, z + dz, BLOCK_STONE);
			}
}

static struct entity* spawn_pig(float x, float y, float z) {
	struct entity* e
		= server_local_spawn_monster_at((vec3) {x, y, z}, MONSTER_PIG, &s);
	assert(e && e->tick_server);
	e->vel[0] = e->vel[1] = e->vel[2] = 0.0F; /* Beta spawns at rest */
	e->data.monster.entity_age = 0;
	return e;
}

static void pit_reset(struct entity* e, int x, int z) {
	e->teleport(e, (vec3) {x + 0.5F, (float)FLOOR_Y, z + 0.5F});
	e->vel[0] = e->vel[1] = e->vel[2] = 0.0F;
	/* "From rest" must also zero the AUTHORITATIVE double motion (mob motion
	 * units S2: server_tick_monster integrates motion_d, not e->vel). Without
	 * this the previous tick's motionY carried over and vel.y > 0 measured
	 * accumulated buoyancy, not this tick's single swim draw -- the e4 guard
	 * then passed or failed on carry-over (P 52/60, LIVING-SOUND-RNG 61/64),
	 * not on the per-entity draw it claims to observe. */
	for(int axis = 0; axis < 3; axis++)
		e->data.monster.motion_d.motion[axis] = 0.0;
	e->data.monster.entity_age = 0;
}

static void tick(struct entity* e) {
	bool reaped = e->tick_server(e, &s);
	assert(!reaped);
}

/* Tick an entity whose ONLY purpose is to consume RNG. The client cosmetic
 * stream (gstate.rand_src: idle-look, particles) is restored around it so the
 * comparison measures the SIMULATION streams only -- that stream is client
 * state and outside this card. */
static void tick_draw_only(struct entity* e) {
	struct random_gen cosmetic = gstate.rand_src;
	tick(e);
	gstate.rand_src = cosmetic;
}

struct pick {
	int cx, cz, y;
};
/* The first position server_world_random_tick would pick from stream `g`
 * (server_world.c: rand_gen_range x, z, then y). */
static struct pick first_pick(struct random_gen g) {
	struct pick p;
	p.cx = rand_gen_range(&g, 0, CHUNK_SIZE);
	p.cz = rand_gen_range(&g, 0, CHUNK_SIZE);
	p.y = rand_gen_range(&g, 0, WORLD_HEIGHT);
	return p;
}

/* ---- (a) ------------------------------------------------------------------- */
static void case_a_world_stream(void) {
	printf("(a) mob swim/wander tick vs world random-tick stream\n");
	/* control: pig present, not ticked */
	fixture_open();
	build_pit(4, 4);
	struct entity* pig = spawn_pig(4.5F, (float)FLOOR_Y, 4.5F);
	s.rand_src.seed = SEED_WORLD_STREAM;
	s.rand_src.has_next_gaussian = false;
	struct pick ctrl_pick = first_pick(s.rand_src);
	server_world_random_tick(&s.worlds[0], &s.rand_src, &s, 0, 0, 1);
	uint32_t ctrl_after = s.rand_src.seed;
	(void)pig;
	fixture_close();

	/* same world, the pig swims one tick first */
	fixture_open();
	build_pit(4, 4);
	pig = spawn_pig(4.5F, (float)FLOOR_Y, 4.5F);
	struct block_data at = {0};
	bool got = server_world_get_block(&s.worlds[0], 4, FLOOR_Y, 4, &at);
	printf("    pit cell: got %d type %d\n", (int)got, (int)at.type);
	assert(got && (at.type == BLOCK_WATER_STILL || at.type == BLOCK_WATER_FLOW)); /* precondition: in water (placing still water beside air re-types it flowing) */
	s.rand_src.seed = SEED_WORLD_STREAM;
	s.rand_src.has_next_gaussian = false;
	tick(pig);
	uint32_t after_mob = s.rand_src.seed;
	printf("    world stream %08x -> %08x across one pig tick\n",
		   (unsigned)SEED_WORLD_STREAM, (unsigned)after_mob);
	red("a1", after_mob == SEED_WORLD_STREAM && !s.rand_src.has_next_gaussian,
		"a swimming pig's tick draws nothing from the world stream "
		"(EntityCreature.java:47/:106, EntityLiving.java:723 use this.rand)");
	struct pick mob_pick = first_pick(s.rand_src);
	server_world_random_tick(&s.worlds[0], &s.rand_src, &s, 0, 0, 1);
	printf("    first random-tick pick control (%d,%d,%d) vs after pig (%d,%d,%d)\n",
		   ctrl_pick.cx, ctrl_pick.y, ctrl_pick.cz, mob_pick.cx, mob_pick.y,
		   mob_pick.cz);
	red("a2",
		mob_pick.cx == ctrl_pick.cx && mob_pick.cz == ctrl_pick.cz
			&& mob_pick.y == ctrl_pick.y && s.rand_src.seed == ctrl_after,
		"world random-tick picks are identical with and without the mob's "
		"draws (the tick-568 coupling is gone)");
	fixture_close();
}

/* ---- (b) ------------------------------------------------------------------- */
#define B_TICKS 400
static void run_b(bool with_a, float out[B_TICKS][3]) {
	fixture_open();
	build_pit(3, 3);
	s.next_entity_id = 100;
	struct entity* b = spawn_pig(26.5F, (float)FEET_Y, 8.5F);
	struct entity* a = with_a ? spawn_pig(3.5F, (float)FLOOR_Y, 3.5F) : NULL;
	/* Same stream state after spawning in both runs: (b) isolates TICK draws
	 * (spawn draws are case e). */
	s.rand_src = (struct random_gen) {.seed = SEED_AFTER_SPAWN};
	gstate.rand_src = (struct random_gen) {.seed = 0xC05E71Cu};
	for(int t = 0; t < B_TICKS; t++) {
		if(a) {
			pit_reset(a, 3, 3);
			tick_draw_only(a);
		}
		b->data.monster.entity_age = 0;
		tick(b);
		out[t][0] = b->pos[0];
		out[t][1] = b->pos[1];
		out[t][2] = b->pos[2];
	}
	fixture_close();
}

static float s_b_ctrl[B_TICKS][3], s_b_with[B_TICKS][3], s_b_again[B_TICKS][3];

static void case_b_independence(void) {
	printf("(b) entity-entity independence (pig A's draws vs pig B)\n");
	run_b(false, s_b_ctrl);
	/* precondition: B actually rolled and moved, or the comparison is empty */
	float moved = 0.0F;
	for(int t = 0; t < B_TICKS; t++) {
		float dx = s_b_ctrl[t][0] - 26.5F, dz = s_b_ctrl[t][2] - 8.5F;
		float d = sqrtf(dx * dx + dz * dz);
		if(d > moved)
			moved = d;
	}
	printf("    control pig B max displacement %.3f blocks\n", moved);
	assert(moved > 0.25F);
	run_b(true, s_b_with);
	int first = -1;
	for(int t = 0; t < B_TICKS && first < 0; t++)
		if(memcmp(s_b_ctrl[t], s_b_with[t], sizeof s_b_ctrl[t]))
			first = t;
	if(first >= 0)
		printf("    first divergence tick %d: (%.6f,%.6f,%.6f) vs (%.6f,%.6f,%.6f)\n",
			   first, s_b_ctrl[first][0], s_b_ctrl[first][1],
			   s_b_ctrl[first][2], s_b_with[first][0], s_b_with[first][1],
			   s_b_with[first][2]);
	red("b1", first < 0,
		"pig B's trajectory is byte-identical whether or not pig A draws "
		"(Entity.java:50: one Random per instance)");

	/* (e2) reproducibility guard, same fixture: a fixed world seed + spawn
	 * order must replay exactly. Holds today; must keep holding. */
	run_b(false, s_b_again);
	assert(!memcmp(s_b_ctrl, s_b_again, sizeof s_b_ctrl));
	printf("  [e2] guard: fixed seed + spawn order replays byte-identically\n");
}

/* ---- (c) ------------------------------------------------------------------- */
#define C_TICKS 64
static void item_reset(struct entity* e, int x, int z) {
	e->teleport(e, (vec3) {x + 0.5F, FEET_Y + 0.3F, z + 0.5F});
	e->vel[0] = e->vel[1] = e->vel[2] = 0.0F;
	e->health = 100; /* survive one lava tick (4 contact + fire DoT) */
	e->data.item.persisted_fire = 0;
	e->data.item.age = 0;
}

static struct entity* spawn_dirt_item(int x, int z) {
	struct item_data it = {.id = BLOCK_DIRT, .durability = 0, .count = 1};
	struct entity* e = server_local_spawn_item(
		(vec3) {x + 0.5F, FEET_Y + 0.3F, z + 0.5F}, &it, false, &s);
	assert(e && e->tick_server);
	return e;
}

static void run_c(bool with_a, float out[C_TICKS][2], uint32_t* world_drift) {
	fixture_open();
	set(26, FEET_Y, 8, BLOCK_LAVA_STILL);
	set(3, FEET_Y, 3, BLOCK_LAVA_STILL);
	s.next_entity_id = 200;
	struct entity* b = spawn_dirt_item(26, 8);
	struct entity* a = with_a ? spawn_dirt_item(3, 3) : NULL;
	s.rand_src = (struct random_gen) {.seed = SEED_AFTER_SPAWN};
	s.entity_rng = (struct random_gen) {.seed = SEED_ITEM_STREAM};
	*world_drift = 0;
	for(int t = 0; t < C_TICKS; t++) {
		if(a) {
			item_reset(a, 3, 3);
			tick_draw_only(a);
		}
		item_reset(b, 26, 8);
		uint32_t w0 = s.rand_src.seed;
		tick(b);
		if(s.rand_src.seed != w0)
			(*world_drift)++;
		/* lava bounce: vel.x = (r1-r2)*0.2F, vel.z = (r3-r4)*0.2F before the
		 * move/drag -- recorded after the tick (a pure function of r1..r4) */
		out[t][0] = b->vel[0];
		out[t][1] = b->vel[2];
	}
	fixture_close();
}

static float s_c_ctrl[C_TICKS][2], s_c_with[C_TICKS][2];

static void case_c_item_lava(void) {
	printf("(c) item-entity lava motion stream\n");
	uint32_t drift_ctrl, drift_with;
	run_c(false, s_c_ctrl, &drift_ctrl);
	/* precondition: the lava branch really fired (nonzero, varying motion) */
	int varied = 0;
	for(int t = 1; t < C_TICKS; t++)
		varied += memcmp(s_c_ctrl[t], s_c_ctrl[t - 1], sizeof s_c_ctrl[t]) != 0;
	assert(varied > C_TICKS / 2);
	/* guard (W1C-b, holds today): item lava rolls never touch the world stream */
	assert(drift_ctrl == 0);
	printf("  [c0] guard: item lava ticks leave s->rand_src untouched\n");
	run_c(true, s_c_with, &drift_with);
	assert(drift_with == 0);
	int first = -1;
	for(int t = 0; t < C_TICKS && first < 0; t++)
		if(memcmp(s_c_ctrl[t], s_c_with[t], sizeof s_c_ctrl[t]))
			first = t;
	if(first >= 0)
		printf("    first divergence tick %d: vel (%.6f,%.6f) vs (%.6f,%.6f)\n",
			   first, s_c_ctrl[first][0], s_c_ctrl[first][1],
			   s_c_with[first][0], s_c_with[first][1]);
	red("c1", first < 0,
		"item B's lava motion (EntityItem.java:46-51, four this.rand draws) is "
		"identical whether or not item A bounces in lava too");
}

/* ---- (d) ------------------------------------------------------------------- */
static void case_d_dispenser(void) {
	printf("(d) dispensed projectile spread stream\n");
	fixture_open();
	const int ids[2] = {ITEM_ARROW, ITEM_SNOW_BALL};
	const char* names[2] = {"arrow", "snowball"};
	for(int k = 0; k < 2; k++) {
		int x = 10 + 2 * k, z = 10;
		assert(server_world_set_block(&s.worlds[0], x, FEET_Y, z,
				(struct block_data) {.type = BLOCK_DISPENSER, .metadata = 5}));
		assert(dispenser_te_register(&s, x, FEET_Y, z) >= 0);
		struct dispenser_te* d = dispenser_te_lookup(&s, x, FEET_Y, z);
		assert(d);
		memset(d->items, 0, sizeof d->items);
		d->items[0] = (struct item_data) {.id = (uint16_t)ids[k], .count = 1};
		d->dispenser_random = (struct random_gen) {.seed = 3};
		s.pending_arrow_count = 0;
		size_t before_entities = dict_entity_size(s.entities);
		s.rand_src = (struct random_gen) {.seed = SEED_WORLD_STREAM};
		server_local_dispenser_dispense(&s, x, FEET_Y, z, 5);
		/* precondition: the projectile really materialized */
		assert(d->items[0].count == 0);
		assert(s.pending_arrow_count == 1
			   || dict_entity_size(s.entities) == before_entities + 1);
		printf("    %s: world stream %08x -> %08x (gaussian cached %d)\n",
			   names[k], (unsigned)SEED_WORLD_STREAM,
			   (unsigned)s.rand_src.seed, (int)s.rand_src.has_next_gaussian);
		char what[160];
		snprintf(what, sizeof what,
				 "dispensing one %s draws nothing from the world stream "
				 "(EntityArrow.java:55 spread = projectile this.rand; "
				 "BlockDispenser var5 only in the item branch)",
				 names[k]);
		red(k == 0 ? "d1" : "d2",
			s.rand_src.seed == SEED_WORLD_STREAM && !s.rand_src.has_next_gaussian,
			what);
	}
	fixture_close();
}

/* ---- (f) I02: exact dispensed-projectile spread on the projectile's stream --
 * CFR BlockDispenser.java:126-140: `new EntityArrow/EntityEgg/EntitySnowball
 * (world, x, y, z)` (no draw in these constructors, EntityArrow.java:50-55,
 * EntityEgg.java:72-78, EntitySnowball.java:70-76), then setArrowHeading /
 * func_20078_a / func_6141_a((double)dx, 0.1F, (double)dz, 1.1F, 6.0F):
 * three `this.rand.nextGaussian()` X, Y, Z (EntityArrow.java:84-86). The
 * projectile's stream here is its lifetime java_random
 * (entity_lifetime_rng_seed: world seed, dimension, entity id). */
static uint32_t newest_entity_id(void) {
	uint32_t best = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s.entities); !dict_entity_end_p(it);
		dict_entity_next(it))
		if(dict_entity_ref(it)->key > best)
			best = dict_entity_ref(it)->key;
	return best;
}

static void expected_heading(uint32_t id, const float aim[3], float speed,
							 float deviation, int discard, float out[3]) {
	struct java_random jr;
	entity_lifetime_rng_seed(&jr, s.worlds[0].seed, s.worlds[0].dimension, id);
	for(int i = 0; i < discard; i++)
		(void)jrand_next_gaussian(&jr);
	float g0 = (float)jrand_next_gaussian(&jr);
	float g1 = (float)jrand_next_gaussian(&jr);
	float g2 = (float)jrand_next_gaussian(&jr);
	vec3 dir = {aim[0], aim[1], aim[2]};
	glm_vec3_normalize(dir);
	arrow_apply_spread(dir, g0, g1, g2, deviation);
	out[0] = dir[0] * speed;
	out[1] = dir[1] * speed;
	out[2] = dir[2] * speed;
}

static bool vel_is(const struct entity* e, const float want[3]) {
	return e && fabsf(e->vel[0] - want[0]) < 1e-6F
		&& fabsf(e->vel[1] - want[1]) < 1e-6F
		&& fabsf(e->vel[2] - want[2]) < 1e-6F;
}

static void case_f_dispenser_exact(void) {
	printf("(f) dispensed projectile: exact Beta spread from its own stream\n");
	fixture_open();
	const int ids[3] = {ITEM_ARROW, ITEM_EGG, ITEM_SNOW_BALL};
	const char* names[3] = {"arrow", "egg", "snowball"};
	for(int k = 0; k < 3; k++) {
		int x = 10 + 2 * k, z = 10;
		assert(server_world_set_block(&s.worlds[0], x, FEET_Y, z,
				(struct block_data) {.type = BLOCK_DISPENSER, .metadata = 5}));
		assert(dispenser_te_register(&s, x, FEET_Y, z) >= 0);
		struct dispenser_te* d = dispenser_te_lookup(&s, x, FEET_Y, z);
		assert(d);
		memset(d->items, 0, sizeof d->items);
		d->items[0] = (struct item_data) {.id = (uint16_t)ids[k], .count = 1};
		d->dispenser_random = (struct random_gen) {.seed = 3};
		s.pending_arrow_count = 0;
		s.rand_src = (struct random_gen) {.seed = SEED_WORLD_STREAM};
		server_local_dispenser_dispense(&s, x, FEET_Y, z, 5);
		server_local_test_flush_arrows(&s);
		assert(d->items[0].count == 0);
		uint32_t id = newest_entity_id();
		struct entity** pp = dict_entity_get(s.entities, id);
		struct entity* e = pp ? *pp : NULL;
		float want[3];
		expected_heading(id, (const float[3]) {1.0F, 0.1F, 0.0F}, 1.1F, 6.0F, 0,
						 want);
		printf("    %s id %u vel (%.7f %.7f %.7f) Beta (%.7f %.7f %.7f)\n",
			   names[k], (unsigned)id, e ? (double)e->vel[0] : 0.0,
			   e ? (double)e->vel[1] : 0.0, e ? (double)e->vel[2] : 0.0,
			   (double)want[0], (double)want[1], (double)want[2]);
		char what[200];
		snprintf(what, sizeof what,
				 "dispensed %s heading = setArrowHeading(dx,0.1F,dz,1.1F,6.0F) "
				 "on its OWN stream; world stream untouched (BlockDispenser"
				 ".java:126-140)",
				 names[k]);
		red(k == 0 ? "f1" : k == 1 ? "f2" : "f3",
			vel_is(e, want) && s.rand_src.seed == SEED_WORLD_STREAM
				&& !s.rand_src.has_next_gaussian,
			what);
	}
	fixture_close();
}

/* ---- (g) I02: skeleton arrow spread on the NEW arrow's stream ------------
 * CFR EntitySkeleton.java:61-68: `new EntityArrow(world, this)` -- whose
 * constructor already calls setArrowHeading(motion, 1.5F, 1.0F) (three
 * gaussians, EntityArrow.java:66) -- then setArrowHeading(dx, dy + d*0.2F,
 * dz, 0.6F, 12.0F): three MORE gaussians, all `var7.rand` (the arrow). The
 * skeleton's own rand only rolls the bow-sound pitch; the world stream is
 * not touched. */
static void case_g_skeleton(void) {
	printf("(g) skeleton arrow spread on the arrow's own stream\n");
	fixture_open();
	s.player.x = 20.5;
	s.player.y = (double)FEET_Y + 1.62;
	s.player.z = 8.5;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = 20 * 8;
	struct entity* sk = server_local_spawn_monster_at(
		(vec3) {14.5F, (float)FEET_Y, 8.5F}, MONSTER_SKELETON, &s);
	assert(sk && sk->tick_server);
	sk->vel[0] = sk->vel[1] = sk->vel[2] = 0.0F;
	bool fired = false, world_clean = false;
	for(int t = 0; t < 120 && !fired; t++) {
		s.pending_arrow_count = 0;
		struct random_gen before = s.rand_src;
		struct random_gen cosmetic = gstate.rand_src;
		bool rm = sk->tick_server(sk, &s);
		gstate.rand_src = cosmetic;
		assert(!rm);
		if(s.pending_arrow_count > 0) {
			fired = true;
			world_clean = memcmp(&before, &s.rand_src, sizeof before) == 0;
		}
	}
	assert(fired && "fixture: the skeleton shot within 120 ticks");
	server_local_test_flush_arrows(&s);
	uint32_t id = newest_entity_id();
	struct entity** pp = dict_entity_get(s.entities, id);
	struct entity* a = pp ? *pp : NULL;
	assert(a && a->type == ENTITY_ARROW);
	/* Undo the spread drawn from the arrow's stream (discard 3, use 3): the
	 * remainder must be the unit aim vector. */
	struct java_random jr;
	entity_lifetime_rng_seed(&jr, s.worlds[0].seed, s.worlds[0].dimension, id);
	for(int i = 0; i < 3; i++)
		(void)jrand_next_gaussian(&jr);
	float g[3];
	for(int i = 0; i < 3; i++)
		g[i] = (float)jrand_next_gaussian(&jr);
	float u[3];
	for(int i = 0; i < 3; i++)
		u[i] = a->vel[i] / ARROW_SPEED
			- g[i] * ARROW_SPREAD_PER_DEVIATION * SKELETON_ARROW_DEVIATION;
	float len = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
	printf("    arrow id %u vel (%.6f %.6f %.6f); unspread |aim| = %.7f; world "
		   "stream %s on the firing tick\n",
		   (unsigned)id, (double)a->vel[0], (double)a->vel[1],
		   (double)a->vel[2], (double)len, world_clean ? "untouched" : "MOVED");
	red("g1", world_clean,
		"a skeleton shot draws nothing from the world stream "
		"(EntitySkeleton.java:61-68: spread is var7.rand)");
	red("g2", fabsf(len - 1.0F) < 1e-5F,
		"the arrow's spread is gaussians 4-6 of the arrow's own stream "
		"(constructor heading 1-3, EntityArrow.java:66 then :84-86)");
	fixture_close();
}

/* ---- (e) ------------------------------------------------------------------- */
#define E_TICKS 64
static void case_e_seeding(void) {
	printf("(e) seeding and lifetime\n");
	fixture_open();
	build_pit(3, 3);
	build_pit(27, 12);

	/* e1: creation draws nothing from the world stream */
	s.rand_src = (struct random_gen) {.seed = SEED_WORLD_STREAM};
	struct entity* p = server_local_spawn_monster_at(
		(vec3) {3.5F, (float)FLOOR_Y, 3.5F}, MONSTER_PIG, &s);
	assert(p);
	printf("    spawn: world stream %08x -> %08x\n",
		   (unsigned)SEED_WORLD_STREAM, (unsigned)s.rand_src.seed);
	red("e1", s.rand_src.seed == SEED_WORLD_STREAM,
		"spawning a mob draws nothing from the world stream (Beta mobs start "
		"at rest; Entity.java:50 new Random(), no World.rand in construction)");
	struct entity* q = spawn_pig(27.5F, (float)FLOOR_Y, 12.5F);

	/* e3 + e4 guards: identical pits, reset every tick; the swim outcome
	 * (vel.y > 0 after one water step from rest) is a per-entity draw. */
	bool seq_p[E_TICKS], seq_q[E_TICKS];
	int swims_p = 0, swims_q = 0;
	for(int t = 0; t < E_TICKS; t++) {
		pit_reset(p, 3, 3);
		tick(p);
		seq_p[t] = p->vel[1] > 0.005F;
		pit_reset(q, 27, 12);
		tick(q);
		seq_q[t] = q->vel[1] > 0.005F;
		/* one water step from rest has exactly two outcomes: the 0.8F swim
		 * draw jumped ((0.04F)*0.8F - 0.02 = +0.012) or did not (-0.02) */
		assert(fabsf(p->vel[1] - 0.012F) < 1e-6F || fabsf(p->vel[1] + 0.02F) < 1e-6F);
		assert(fabsf(q->vel[1] - 0.012F) < 1e-6F || fabsf(q->vel[1] + 0.02F) < 1e-6F);
		swims_p += seq_p[t];
		swims_q += seq_q[t];
	}
	printf("    swim outcomes over %d ticks: p %d, q %d\n", E_TICKS, swims_p,
		   swims_q);
	/* precondition: the observable works (Beta 0.8 swim chance) */
	assert(swims_p > 0 && swims_q > 0);
	/* e4: no per-tick reseed -- a reseeded stream would repeat one outcome */
	assert(swims_p < E_TICKS && swims_q < E_TICKS);
	printf("  [e4] guard: per-entity outcomes vary tick to tick (no reseed)\n");
	/* e3: distinct entities, distinct streams */
	assert(memcmp(seq_p, seq_q, sizeof seq_p) != 0);
	printf("  [e3] guard: two identically placed entities draw different "
		   "sequences\n");
	fixture_close();
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(clin_init());
	assert(svin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	case_a_world_stream();
	case_b_independence();
	case_c_item_lava();
	case_d_dispenser();
	case_e_seeding();
	case_f_dispenser_exact();
	case_g_skeleton();

	printf("RED SUMMARY: %d of %d per-entity RNG checks open\n", red_open,
		   red_total);
	if(red_open) {
		printf("test_entity_rng_streams: RED as expected (WILL_FAIL) -- the "
			   "per-entity RNG card is not implemented yet\n");
		return 1;
	}
	printf("test_entity_rng_streams: all checks GREEN -- drop WILL_FAIL\n");
	return 0;
}
