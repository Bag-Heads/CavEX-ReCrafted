/*
	Mob motion units, slice S1 (docs/intent/mob-motion-units.md; Astra rulings
	2026-09-27): the authoritative double state + the double Entity.moveEntity
	port + the mutation API, INACTIVE (the live tick still integrates floats).

	Every expected value is written as Beta's OWN expression, with its casts
	spelled out, evaluated here independently of the code under test
	(decompile 740c583, ../mcb173-retromcp src):
	  Entity.setPosition:126-132  box = pos +- (double)(width/2.0F), minY =
	      posY - (double)yOffset + (double)ySize, maxY = minY + (double)height
	  Entity.moveEntity:252-434  ySize *= 0.4F; web: dx *= 0.25D, dy *=
	      (double)0.05F, dz *= 0.25D, motion = 0; Y/X/Z clip
	      (AxisAlignedBB.calculate*Offset); stepHeight retry; posY = minY +
	      (double)yOffset - (double)ySize; onGround = requested dy != dy && requested dy <
	      0; motion zeroed per clipped axis.
	  EntityLiving.moveEntityWithHeading:527-535  (motionY - 0.08D) *
	      (double)0.98F  (used for the free-fall precision vector)

	Cases (argv[1]):
	  aabb        calculateX/Y/ZOffset, intersectsWith, addCoord
	  fall        landing clip: dy clipped to the floor face, onGround, motionY 0
	  precision   a free-fall step lands on Beta's double, not the float sum
	  wall        X clip to face contact; the step retry keeps the flat move
	  step        slab step-up: longer move kept, ySize banked 0.51F, onGround
	              false (Beta's requested-dy == final-dy quirk)
	  web         web scaling with (double)0.05F and motion zeroed
	  seed        constructor + teleport seed the double state (setPosition box)
	  mirror      inactive API writes are bit-identical to the legacy float
	              writes; a direct float write is counted as a bypass
	  live_tick   40 real server ticks of a lone mob: no bypass, and the
	              inactive double state equals (double) of the float result
	  blast_route RED until the lane A handoff routes server_world.c's blast
	              impulse through mob_motion_add_velocity
	  push_route  RED until the lane A handoff routes entity.c's
	              applyEntityCollision push through the API
	  size        prints sizeof(struct entity) / sizeof(struct mob_motion) and
	              the inactive tick-bracket cost (measurement, no assert)
*/
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/mob_motion.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "util.h"
#include "support/drain_quiesce.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define WY 100

static struct server_local s;
static int g_fail;

#define CHECK(c)                                                               \
	do {                                                                       \
		if(!(c)) {                                                             \
			fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); \
			g_fail = 1;                                                        \
		}                                                                      \
	} while(0)

static volatile int g_drain_run = 1;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		test_drain_pass();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, int type) {
	server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type, .metadata = 0});
}

static uint32_t g_next_id = 700000;

/* A zombie constructed through the production constructor and teleported
 * (entity_monster_teleport) -- the spawn path shape. */
/* A PIG: the fixture's difficulty is 0 (peaceful), which removes hostile
 * mobs on their tick; a passive mob lives through the real ticks below. */
static int g_mob_kind = MONSTER_PIG;
static struct entity* make_mob(double x, double y, double z, bool join) {
	uint32_t id = g_next_id++;
	struct entity* e;
	if(join) {
		struct entity** pp = dict_entity_safe_get(s.entities, id);
		*pp = malloc(sizeof(**pp));
		assert(*pp);
		e = *pp;
	} else {
		e = malloc(sizeof(*e));
		assert(e);
	}
	memset(e, 0, sizeof(*e));
	entity_monster(id, e, true, &s.worlds[0], g_mob_kind);
	e->teleport(e, (vec3) {(float)x, (float)y, (float)z});
	return e;
}

static struct mob_motion* md(struct entity* e) {
	return &e->data.monster.motion_d;
}

/* ---- aabb ---------------------------------------------------------------- */
static void case_aabb(void) {
	struct mob_aabb_d block = {{12.0, 101.0, 10.0}, {13.0, 102.0, 11.0}};
	struct mob_aabb_d mover = {{11.2, 101.0, 10.2}, {11.8, 102.8, 10.8}};
	/* +X toward the block: 12.0 - 11.8 */
	CHECK(mob_aabb_calc_x_offset(&block, &mover, 0.5) == 12.0 - 11.8);
	/* moving away: untouched */
	CHECK(mob_aabb_calc_x_offset(&block, &mover, -0.5) == -0.5);
	/* no Z overlap -> untouched */
	struct mob_aabb_d off = mover;
	off.min[2] = 11.0;
	off.max[2] = 11.6;
	CHECK(mob_aabb_calc_x_offset(&block, &off, 0.5) == 0.5);
	/* Y: mover above a floor block */
	struct mob_aabb_d floor = {{11.0, 100.0, 10.0}, {12.0, 101.0, 11.0}};
	struct mob_aabb_d above = {{11.2, 101.25, 10.2}, {11.8, 103.05, 10.8}};
	CHECK(mob_aabb_calc_y_offset(&floor, &above, -0.5) == 101.0 - 101.25);
	/* Z */
	struct mob_aabb_d zb = {{11.0, 101.0, 12.0}, {12.0, 102.0, 13.0}};
	CHECK(mob_aabb_calc_z_offset(&zb, &mover, 2.0) == 12.0 - 10.8);
	/* intersectsWith is strict: shared face is not an intersection */
	struct mob_aabb_d touching = {{13.0, 101.0, 10.0}, {14.0, 102.0, 11.0}};
	CHECK(!mob_aabb_intersects(&block, &touching));
	CHECK(mob_aabb_intersects(&block, &block));
	/* addCoord extends by sign */
	struct mob_aabb_d a = mob_aabb_add_coord(&mover, 0.5, -0.25, 0.0);
	CHECK(a.max[0] == 11.8 + 0.5 && a.min[0] == 11.2);
	CHECK(a.min[1] == 101.0 - 0.25 && a.max[1] == 102.8);
	puts("  aabb: calculateX/Y/ZOffset, strict intersectsWith, addCoord OK");
}

/* ---- fall ---------------------------------------------------------------- */
static void case_fall(void) {
	set(10, WY, 10, BLOCK_STONE);
	struct entity* e = make_mob(10.5, 101.25, 10.5, false);
	struct mob_motion* m = md(e);
	e->on_ground = false;
	m->motion[1] = -0.5;
	struct mob_move_result r = mob_move_entity_d(e, m, 0.0, -0.5, 0.0, NULL);
	CHECK(!r.incomplete);
	CHECK(r.dy == 101.0 - 101.25);
	CHECK(m->pos[1] == 101.0);
	CHECK(m->bb.min[1] == 101.0);
	CHECK(e->on_ground && r.on_ground);
	CHECK(e->collided_vertically && !e->collided_horizontally);
	CHECK(m->motion[1] == 0.0);
	free(e);
	puts("  fall: dy clipped to the floor face, onGround, motionY zeroed OK");
}

/* ---- precision --------------------------------------------------------------- */
static void case_precision(void) {
	struct entity* e = make_mob(40.5, 110.3, 40.5, false);
	struct mob_motion* m = md(e);
	/* Beta's first free-fall step from rest: (0 - 0.08D) * (double)0.98F. */
	const double dy = (0.0 - 0.08) * (double)0.98F;
	const double y0 = m->pos[1];
	CHECK(y0 == (double)110.3F); /* seeded from the float teleport */
	e->on_ground = false;
	mob_move_entity_d(e, m, 0.0, dy, 0.0, NULL);
	/* minY = posY - (double)yOffset + (double)ySize (0); posY = minY + dy'
	 * + (double)yOffset - (double)ySize. */
	double minY = y0 - (double)e->y_offset + (double)0.0F;
	minY += dy;
	const double want = minY + (double)e->y_offset - (double)e->y_size;
	CHECK(m->pos[1] == want);
	/* the float path's sum is a different number */
	const float fsum = (float)y0 + (float)dy;
	CHECK((double)fsum != want);
	free(e);
	puts("  precision: a free-fall step lands on Beta's double, not the float "
		 "sum OK");
}

/* ---- wall ------------------------------------------------------------------- */
static void case_wall(void) {
	for(int x = 10; x <= 12; x++)
		set(x, WY, 20, BLOCK_STONE);
	set(12, WY + 1, 20, BLOCK_STONE);
	set(12, WY + 2, 20, BLOCK_STONE);
	struct entity* e = make_mob(11.5, 101.0, 20.5, false);
	struct mob_motion* m = md(e);
	e->on_ground = true;
	m->motion[0] = 0.3;
	const double maxX = 11.5 + (double)(e->width / 2.0F);
	mob_move_entity_d(e, m, 0.3, 0.0, 0.0, NULL);
	const double dx = 12.0 - maxX;
	CHECK(m->bb.max[0] == maxX + dx);
	CHECK(m->pos[1] == 101.0); /* the 0.5 step retry was shorter-or-equal */
	CHECK(e->collided_horizontally);
	CHECK(m->motion[0] == 0.0);
	CHECK(e->y_size == 0.0F);
	free(e);
	puts("  wall: X clipped to face contact, step retry discarded OK");
}

/* ---- step ------------------------------------------------------------------- */
static void case_step(void) {
	for(int x = 10; x <= 13; x++)
		set(x, WY, 30, BLOCK_STONE);
	set(12, WY + 1, 30, BLOCK_SLAB);
	struct entity* e = make_mob(11.5, 101.0, 30.5, false);
	struct mob_motion* m = md(e);
	e->on_ground = true;
	const double minX0 = 11.5 - (double)(e->width / 2.0F);
	mob_move_entity_d(e, m, 0.3, 0.0, 0.0, NULL);
	/* stepped: box raised 0.5 onto the slab top, full 0.3 kept */
	CHECK(m->bb.min[1] == 101.5);
	CHECK(m->bb.min[0] == minX0 + 0.3);
	/* ySize = (float)((double)ySize + var41 + 0.01D), var41 = 101.5 - 101 */
	const float ys = (float)((double)0.0F + (101.5 - (double)(int32_t)101.5)
							 + 0.01);
	CHECK(e->y_size == ys);
	CHECK(m->pos[1] == 101.5 + (double)e->y_offset - (double)ys);
	/* var13 (0) == var3 (0): not onGround, not collided vertically */
	CHECK(!e->on_ground && !e->collided_vertically);
	CHECK(!e->collided_horizontally);
	free(e);
	puts("  step: slab step-up keeps the longer move, ySize 0.51F, onGround "
		 "false OK");
}

/* ---- web -------------------------------------------------------------------- */
static void case_web(void) {
	struct entity* e = make_mob(50.5, 115.0, 50.5, false);
	struct mob_motion* m = md(e);
	e->in_web = true;
	e->on_ground = false;
	m->motion[0] = 0.4;
	m->motion[1] = -0.4;
	m->motion[2] = 0.4;
	const double x0 = m->pos[0], y0 = m->pos[1];
	struct mob_move_result r = mob_move_entity_d(e, m, 0.4, -0.4, 0.4, NULL);
	CHECK(r.dx == 0.4 * 0.25);
	CHECK(r.dy == -0.4 * (double)0.05F);
	CHECK(!e->in_web);
	CHECK(m->motion[0] == 0.0 && m->motion[1] == 0.0 && m->motion[2] == 0.0);
	(void)x0;
	(void)y0;
	free(e);
	puts("  web: 0.25D / (double)0.05F scaling, motion zeroed OK");
}

/* ---- seed ------------------------------------------------------------------- */
static void case_seed(void) {
	struct entity* e = make_mob(20.25, 101.0, 20.75, false);
	struct mob_motion* m = md(e);
	CHECK(m->valid);
	CHECK(m->pos[0] == (double)20.25F && m->pos[1] == 101.0
		  && m->pos[2] == (double)20.75F);
	CHECK(memcmp(m->prev_pos, m->pos, sizeof m->pos) == 0);
	CHECK(e->y_size == 0.0F);
	const float half = e->width / 2.0F;
	CHECK(m->bb.min[0] == m->pos[0] - (double)half);
	CHECK(m->bb.max[2] == m->pos[2] + (double)half);
	CHECK(m->bb.min[1] == m->pos[1] - (double)e->y_offset + (double)0.0F);
	CHECK(m->bb.max[1] == m->bb.min[1] + (double)e->height);
	CHECK(mob_motion_mirror_matches(e, m));
	/* a second teleport re-seeds */
	e->teleport(e, (vec3) {30.5f, 105.0f, 31.5f});
	CHECK(m->pos[0] == 30.5 && m->pos[1] == 105.0 && m->pos[2] == 31.5);
	CHECK(memcmp(m->prev_pos, m->pos, sizeof m->pos) == 0);
	free(e);
	puts("  seed: constructor + teleport seed pos/prevPos/box OK");
}

/* ---- mirror ----------------------------------------------------------------- */
static void case_mirror(void) {
	/* S2 (ACTIVE): the doubles are authoritative. Every API write lands on
	 * the double first and the float is its single rounding; a direct float
	 * write is counted by the tick bracket and DISCARDED (the float is
	 * restored from the double -- floats never feed back, Astra Q1). */
	struct entity* e = make_mob(20.5, 101.0, 20.5, false);
	struct mob_motion* m = md(e);
	mob_motion_set_velocity(e, m, 0.137, -0.0784, 0.071);
	CHECK(m->motion[0] == 0.137 && m->motion[1] == -0.0784
		  && m->motion[2] == 0.071);
	CHECK(e->vel[0] == (float)0.137 && e->vel[1] == (float)-0.0784);
	mob_motion_bypass_reset();

	/* Entity.addVelocity: motion += d in double, every axis */
	const double d[3] = {0.1234567891, 0.3999999, -0.0000123};
	double want[3] = {0.137 + d[0], -0.0784 + d[1], 0.071 + d[2]};
	mob_motion_add_velocity(e, m, d[0], d[1], d[2]);
	for(int i = 0; i < 3; i++) {
		CHECK(m->motion[i] == want[i]);
		CHECK(e->vel[i] == (float)want[i]);
	}
	mob_motion_tick_begin(e, m);
	CHECK(mob_motion_bypass_count() == 0);

	/* two adds accumulate in double (Beta), not through a float rounding */
	mob_motion_add_velocity(e, m, 1.0e-9, 0.0, 0.0);
	mob_motion_add_velocity(e, m, 1.0e-9, 0.0, 0.0);
	CHECK(m->motion[0] == (want[0] + 1.0e-9) + 1.0e-9);

	/* a direct float write is a bypass, and it is discarded */
	double keep = m->motion[1];
	e->vel[1] += 0.25f;
	mob_motion_tick_begin(e, m);
	CHECK(mob_motion_bypass_count() == 1);
	CHECK(m->motion[1] == keep && e->vel[1] == (float)keep);
	/* ... the mirror is restored, so it does not cascade */
	mob_motion_tick_begin(e, m);
	CHECK(mob_motion_bypass_count() == 1);
	free(e);
	puts("  mirror: API writes land on the doubles (float = one rounding); a "
		 "direct float write is counted and discarded OK");
}

/* ---- live tick ----------------------------------------------------------------- */
/* A host body 20 blocks off (Beta despawnEntity keeps a mob within 32 of
 * the closest player; with no positioned player the fixture mob would be
 * reaped as out of range). */
static void host_near(double x, double y, double z) {
	s.player.x = x + 20.0;
	s.player.y = y;
	s.player.z = z;
	s.player.has_pos = true;
}

static void clear_area(int x0, int z0) {
	for(int x = x0 - 4; x <= x0 + 4; x++)
		for(int z = z0 - 4; z <= z0 + 4; z++) {
			set(x, WY, z, BLOCK_STONE);
			for(int y = WY + 1; y <= WY + 4; y++)
				set(x, y, z, BLOCK_AIR);
		}
}

static void case_live_tick(void) {
	clear_area(60, 60);
	struct entity* e = make_mob(60.5, 103.0, 60.5, true);
	host_near(60.5, 101.0, 60.5);
	mob_motion_bypass_reset();
	for(int t = 0; t < 40; t++) {
		assert(server_local_test_tick_entities(&s));
		struct entity** pp = dict_entity_get(s.entities, e->id);
		CHECK(pp && *pp); /* the pig must live through every tick */
		if(!pp || !*pp)
			break;
		struct mob_motion* m = md(*pp);
		for(int i = 0; i < 3; i++) {
			/* S2: the float mirror is the single rounding of the double */
			CHECK((float)m->pos[i] == (*pp)->pos[i]);
			CHECK((float)m->motion[i] == (*pp)->vel[i]);
		}
	}
	CHECK(mob_motion_bypass_count() == 0);
	printf("  live_tick: 40 real ticks, bypasses=%u, floats mirror the "
		   "doubles OK\n",
		   mob_motion_bypass_count());
}

/* ---- external routing (RED until the lane A handoffs) ----------------------- */
static int case_blast_route(void) {
	clear_area(80, 80);
	struct entity* e = make_mob(80.5, 101.0, 80.5, true);
	host_near(80.5, 101.0, 80.5);
	assert(server_local_test_tick_entities(&s)); /* settle, bracket once */
	mob_motion_bypass_reset();
	server_world_explode(&s.worlds[0], (vec3) {82.5f, 101.5f, 80.5f}, 2.0f);
	struct entity** pp = dict_entity_get(s.entities, e->id);
	if(!pp || !*pp) {
		fprintf(stderr, "blast_route: mob gone\n");
		return 1;
	}
	if(!mob_motion_mirror_matches(*pp, md(*pp))) {
		fprintf(stderr, "RED: server_world.c blast impulse bypasses the mob "
						"motion API (lane A handoff MOTION-S1)\n");
		return 1;
	}
	puts("  blast_route: blast impulse reaches the double state OK");
	return 0;
}

static int case_push_route(void) {
	clear_area(100, 100);
	struct entity* a = make_mob(100.5, 101.0, 100.5, true);
	host_near(100.5, 101.0, 100.5);
	struct entity* b = make_mob(100.7, 101.0, 100.6, true);
	uint32_t aid = a->id, bid = b->id;
	/* one tick registers both bodies' chunk membership (the entity query
	 * snapshot the push loop walks lists only added_to_chunk bodies) */
	assert(server_local_test_tick_entities(&s));
	mob_motion_bypass_reset();
	/* now a's tick pushes b (applyEntityCollision) and vice versa */
	assert(server_local_test_tick_entities(&s));
	assert(server_local_test_tick_entities(&s));
	struct entity** pp = dict_entity_get(s.entities, bid);
	CHECK(pp && *pp);
	if(!pp || !*pp)
		return 1;
	fprintf(stderr, "push_route: a.added=%d b.added=%d b.vel=(%g,%g,%g)\n",
			(int)(dict_entity_get(s.entities, aid) && *dict_entity_get(s.entities, aid)), pp && *pp ? (int)(*pp)->added_to_chunk : -1,
			pp && *pp ? (*pp)->vel[0] : 0.0, pp && *pp ? (*pp)->vel[1] : 0.0,
			pp && *pp ? (*pp)->vel[2] : 0.0);
	if(mob_motion_bypass_count() != 0) {
		fprintf(stderr, "RED: %u bypass(es): entity.c applyEntityCollision "
						"writes mob motion directly (lane A handoff "
						"MOTION-S1)\n",
				mob_motion_bypass_count());
		return 1;
	}
	puts("  push_route: entity push reaches the double state OK");
	return 0;
}

/* ---- size / cost ---------------------------------------------------------------- */
static void case_size(void) {
	printf("  size: sizeof(struct entity)=%zu sizeof(struct mob_motion)=%zu\n",
		   sizeof(struct entity), sizeof(struct mob_motion));
	struct entity* e = make_mob(20.5, 101.0, 20.5, false);
	const int N = 1000000;
	clock_t c0 = clock();
	for(int i = 0; i < N; i++) {
		mob_motion_tick_begin(e, md(e));
		mob_motion_tick_end(e, md(e));
	}
	clock_t c1 = clock();
	printf("  size: inactive tick bracket %.1f ns/mob-tick (PC, clock())\n",
		   (double)(c1 - c0) * 1e9 / CLOCKS_PER_SEC / N);
	free(e);
}

int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : "";
	char tag[64];
	snprintf(tag, sizeof tag, "test_mob_motion_%s_tmp", which);
	test_fixture_dir(tag, s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	/* Overworld-only fixture: worlds[0] named explicitly (world alias gate). */
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 0;
	s.rand_src.seed = 0x6D0B1E55u;
	dict_entity_init(s.entities);
	for(int cz = -2; cz <= 9; cz++)
		for(int cx = -2; cx <= 9; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	s.player.x = 5000.0;
	s.player.y = 5000.0;
	s.player.z = 5000.0;

	int rc = 0;
	if(!strcmp(which, "aabb"))
		case_aabb();
	else if(!strcmp(which, "fall"))
		case_fall();
	else if(!strcmp(which, "precision"))
		case_precision();
	else if(!strcmp(which, "wall"))
		case_wall();
	else if(!strcmp(which, "step"))
		case_step();
	else if(!strcmp(which, "web"))
		case_web();
	else if(!strcmp(which, "seed"))
		case_seed();
	else if(!strcmp(which, "mirror"))
		case_mirror();
	else if(!strcmp(which, "live_tick"))
		case_live_tick();
	else if(!strcmp(which, "blast_route"))
		rc = case_blast_route();
	else if(!strcmp(which, "push_route"))
		rc = case_push_route();
	else if(!strcmp(which, "size"))
		case_size();
	else {
		fprintf(stderr, "unknown case '%s'\n", which);
		rc = 2;
	}

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	if(g_fail)
		return 1;
	if(rc)
		return rc;
	printf("test_mob_motion %s: PASS\n", which);
	return 0;
}
