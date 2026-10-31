/*
	Beta 1.7.3 minecart OFF-RAIL physics goldens (G1; ASTRA-CART-RULING item 5).
	Authored BEFORE the fix (beta-tick-golden discipline). Expected RED on the
	current engine, which stops any cart that is not on a rail
	(entity_minecart.c minecart_server_tick: "no rail -> speed = 0, return").
	Every expected value is computed here from the decompile's arithmetic.

	GROUND TRUTH -- CFR server decompile, /c/tmp/mcb173-server-decompile/out/
	net/minecraft/src/ (full citations: G1-citations.md):
	  EntityMinecart.java:66-67  setSize(0.98F, 0.7F); yOffset = 0.35F
	  EntityMinecart.java:211    motionY -= (double)0.04f       (EVERY tick)
	  EntityMinecart.java:212-215 rail probe: feet cell, else ONE BELOW; never
	                             above
	  EntityMinecart.java:397-421 OFF-RAIL:
	      clamp motionX, motionZ to [-0.4, 0.4]            (motionY unclamped)
	      if (onGround)  motion *= 0.5          (flag from the PREVIOUS move)
	      moveEntity(motionX, motionY, motionZ)
	      if (!onGround) motion *= (double)0.95f (flag from THIS move)
	  Entity.java:257-470 moveEntity: clip Y, then X, then Z; onGround =
	      Y clipped && dy < 0; a clipped axis zeroes its motion; no step-up.

	ENGINE MAPPING (G1-intent.md): an off-rail cart carries Beta's motion
	vector in e->vel (blocks/tick) and Beta's onGround in e->on_ground;
	e->pos is Beta's posX/posY/posZ (box minY = posY - 0.35). Positions are
	floats here, doubles in Beta: expectations are Beta's double recurrence
	started from the engine's float start state, compared within TOL; a
	zeroed axis and the onGround flag are compared exactly.

	Drop-in: tests/test_minecart_offrail_golden.c, linked like
	test_minecart_rail_collision_golden (same harness), +1 count gate.
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
#include "entity/entity.h"
#include "entity/entity_minecart.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD
#define YF 100      /* stone floor; its top face is y = 101 */
#define FLOOR 101.0 /* box minY of a cart resting on the floor */
#define RY 101      /* rail plane (on the floor) */
#define TOL 1e-4

extern struct item item_minecart;

static struct server_local s;
static volatile int g_drain_run = 1;
static int failures;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

#define CHECK(cond, ...) do { \
	if(!(cond)) { \
		fprintf(stderr, "FAIL: " __VA_ARGS__); \
		fprintf(stderr, "\n"); \
		failures++; \
	} \
} while(0)

static void set(int x, int y, int z, int type, int meta) {
	(void)server_world_set_block_silent(&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type,
							 .metadata = (uint8_t)(meta & 0x0F)});
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	assert(b.type == (uint8_t)type && (b.metadata & 0x0F) == (meta & 0x0F));
}

static void clear_carts(void) {
	bool again = true;
	while(again) {
		again = false;
		dict_entity_it_t it;
		dict_entity_it(it, s.entities);
		while(!dict_entity_end_p(it)) {
			struct entity* e = dict_entity_ref(it)->value;
			if(e && e->type == ENTITY_MINECART) {
				dict_entity_erase(s.entities, e->id);
				again = true;
				break;
			}
			dict_entity_next(it);
		}
	}
}

static struct entity* only_minecart(void) {
	struct entity* found = NULL;
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MINECART) {
			assert(!found);
			found = e;
		}
		dict_entity_next(it);
	}
	assert(found);
	return found;
}

/* An OFF-RAIL cart at an exact Beta state. */
static struct entity* offrail_cart(float x, float y, float z, float vx, float vy,
								   float vz, bool on_ground) {
	clear_carts();
	struct entity* e = server_local_spawn_minecart((vec3) {x, y, z}, &s);
	assert(e);
	e = only_minecart();
	e->pos[0] = x;
	e->pos[1] = y;
	e->pos[2] = z;
	glm_vec3_copy(e->pos, e->pos_old);
	e->vel[0] = vx;
	e->vel[1] = vy;
	e->vel[2] = vz;
	e->on_ground = on_ground;
	e->data.minecart.speed = 0.0F;
	e->data.minecart.hx = e->data.minecart.hz = 0;
	e->data.minecart.occupied = false;
	e->data.minecart.occupant_id = 0;
	e->data.minecart.pending_throttle = 0;
	return e;
}

/* ---- Beta, in Beta's arithmetic ---------------------------------------- */
static const double G = (double)0.04f;     /* EntityMinecart.java:211 */
static const double AIR = (double)0.95f;   /* :416-420 */
static const double HALF = (double)(0.98F / 2.0F);
static const double YOFF = (double)(0.7F / 2.0F);

struct beta {
	double x, y, z, mx, my, mz;
	bool on_ground;
};

/* One off-rail tick against: the floor top FLOOR, and an optional full-block
 * wall whose -X face is wall_x (wall_x <= 0: none). Y, then X, then Z. */
static void beta_offrail_tick(struct beta* b, double wall_x) {
	b->my -= G;
	if(b->mx < -0.4) b->mx = -0.4;
	if(b->mx > 0.4) b->mx = 0.4;
	if(b->mz < -0.4) b->mz = -0.4;
	if(b->mz > 0.4) b->mz = 0.4;
	if(b->on_ground) {
		b->mx *= 0.5;
		b->my *= 0.5;
		b->mz *= 0.5;
	}
	double dx = b->mx, dy = b->my, dz = b->mz;
	/* FACE_EPS: the state is seeded from the engine's FLOAT position, so a
	 * box Beta (in doubles) holds exactly on a face can read up to a float
	 * rounding (~2e-6 at y = 101) past it -- e.g. posY 101.35f = 101.349998,
	 * box minY 100.999998. Beta's calculateYOffset/XOffset only clip a box
	 * that is not already past the face; seeded literally, the model would
	 * let a resting cart sink through the floor. A box within FACE_EPS of
	 * the face is the face, as Beta's double state would have it. */
	const double FACE_EPS = 1e-5;
	/* Y vs the floor */
	double miny = b->y - YOFF;
	double cy = dy;
	if(dy < 0.0 && miny >= FLOOR - FACE_EPS && miny + dy < FLOOR)
		cy = FLOOR - miny;
	/* X vs the wall (only a +X move can reach its -X face) */
	double cx = dx;
	if(wall_x > 0.0 && dx > 0.0 && b->x + HALF <= wall_x + FACE_EPS
	   && b->x + HALF + dx > wall_x)
		cx = wall_x - (b->x + HALF);
	b->on_ground = cy != dy && dy < 0.0;
	if(cy != dy)
		b->my = 0.0;
	if(cx != dx)
		b->mx = 0.0;
	b->x += cx;
	b->y += cy;
	b->z += dz;
	if(!b->on_ground) {
		b->mx *= AIR;
		b->my *= AIR;
		b->mz *= AIR;
	}
}

static struct beta from_engine(const struct entity* e) {
	return (struct beta) {(double)e->pos[0], (double)e->pos[1], (double)e->pos[2],
		(double)e->vel[0], (double)e->vel[1], (double)e->vel[2], e->on_ground};
}

static bool near(double a, double b) {
	return fabs(a - b) <= TOL;
}

static void compare(const char* label, int tick, const struct entity* e,
					const struct beta* b) {
	CHECK(near(e->pos[0], b->x) && near(e->pos[1], b->y) && near(e->pos[2], b->z),
		  "%s t%d: pos (%.6f,%.6f,%.6f) Beta (%.6f,%.6f,%.6f)", label, tick,
		  (double)e->pos[0], (double)e->pos[1], (double)e->pos[2], b->x, b->y, b->z);
	CHECK(near(e->vel[0], b->mx) && near(e->vel[1], b->my) && near(e->vel[2], b->mz),
		  "%s t%d: motion (%.6f,%.6f,%.6f) Beta (%.6f,%.6f,%.6f)", label, tick,
		  (double)e->vel[0], (double)e->vel[1], (double)e->vel[2], b->mx, b->my,
		  b->mz);
	CHECK(e->on_ground == b->on_ground, "%s t%d: onGround %d Beta %d", label, tick,
		  e->on_ground, b->on_ground);
}

static void tick_once(void) {
	assert(server_local_test_tick_entities(&s));
}

int main(void) {
	setvbuf(stderr, NULL, _IONBF, 0);
	test_fixture_dir("test_minecart_offrail_golden_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	dict_entity_init(s.entities);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.worlds[0].local = &s;
	struct server_chunk* sc = NULL;
	for(int cx = -2; cx <= 2; cx++)
		for(int cz = -2; cz <= 2; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
	/* a flat stone floor with open air above it (no rail anywhere unless a
	 * case places one) */
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++) {
			set(x, YF, z, BLOCK_STONE, 0);
			for(int y = YF + 1; y <= YF + 8; y++)
				set(x, y, z, BLOCK_AIR, 0);
		}

	/* (A) Free fall from rest, 6 m above the floor, to landing (gravity
	 * :211, air damping *0.95f :416-420, Y clip onto the floor). 6 m, not 3:
	 * motionY passes 0.4 (tick 15) before landing, so the X/Z-only clamp
	 * (:398-409 never clamps Y) is exercised too. */
	{
		struct entity* e = offrail_cart(8.5F, (float)(FLOOR + YOFF + 6.0), 8.5F,
										0, 0, 0, false);
		struct beta b = from_engine(e);
		bool landed = false;
		for(int t = 1; t <= 40 && !landed; t++) {
			beta_offrail_tick(&b, 0);
			tick_once();
			e = only_minecart();
			compare("(A) free fall", t, e, &b);
			landed = b.on_ground;
		}
		CHECK(landed && e->vel[1] == 0.0F && near(e->pos[1], FLOOR + YOFF),
			  "(A) landed: y %.6f motionY %.6f (Beta lands at %.6f, motionY 0)",
			  (double)e->pos[1], (double)e->vel[1], FLOOR + YOFF);
		printf("  (A) free fall to the floor\n");
	}

	/* (B) Airborne horizontal motion: the 0.4 clamp on X/Z (not Y) BEFORE the
	 * move, then *0.95f after it (:398-409, :416-420). */
	{
		struct entity* e = offrail_cart(4.5F, (float)(FLOOR + YOFF + 6.0), 4.5F,
										0.9F, 0.0F, -0.3F, false);
		struct beta b = from_engine(e);
		for(int t = 1; t <= 4; t++) {
			beta_offrail_tick(&b, 0);
			tick_once();
			e = only_minecart();
			compare("(B) air clamp+drag", t, e, &b);
		}
		printf("  (B) airborne clamp and drag\n");
	}

	/* (C) Ground damping: a grounded cart halves ALL motion before the move,
	 * using the PREVIOUS move's onGround, and is not air-damped (:410-414). */
	{
		struct entity* e = offrail_cart(3.5F, (float)(FLOOR + YOFF), 12.5F, 0, 0, 0,
										false);
		tick_once(); /* settle: the Y clip on the floor sets onGround */
		e = only_minecart();
		CHECK(e->on_ground && near(e->pos[1], FLOOR + YOFF),
			  "(C) settle: onGround %d y %.6f", e->on_ground, (double)e->pos[1]);
		e->vel[0] = 0.2F;
		e->vel[2] = -0.1F;
		struct beta b = from_engine(e);
		for(int t = 1; t <= 4; t++) {
			beta_offrail_tick(&b, 0);
			tick_once();
			e = only_minecart();
			compare("(C) ground damping", t, e, &b);
		}
		printf("  (C) ground damping\n");
	}

	/* (D) Clipping: a grounded cart pushed into a stone wall stops at the wall
	 * face and its X motion is zeroed; Z is untouched (moveEntity :398-406). */
	{
		set(12, RY, 4, BLOCK_STONE, 0);
		set(12, RY + 1, 4, BLOCK_STONE, 0);
		struct entity* e = offrail_cart(11.4F, (float)(FLOOR + YOFF), 4.5F, 0, 0, 0,
										false);
		tick_once();
		e = only_minecart();
		e->vel[0] = 0.3F;
		e->vel[2] = 0.05F;
		struct beta b = from_engine(e);
		beta_offrail_tick(&b, 12.0);
		tick_once();
		e = only_minecart();
		compare("(D) wall clip", 1, e, &b);
		CHECK(e->vel[0] == 0.0F, "(D) clipped X motion zeroed (got %.6f)",
			  (double)e->vel[0]);
		CHECK(near(e->pos[0], 12.0 - HALF), "(D) box max X on the wall face: x %.6f",
			  (double)e->pos[0]);
		printf("  (D) wall clip\n");
	}

	/* (E) Derail: a cart rolling off an open rail end keeps moving under the
	 * off-rail rules instead of freezing (the O08/O09 G1 reproducer). */
	{
		set(6, RY, 6, BLOCK_RAIL, 0);
		set(6, RY, 7, BLOCK_RAIL, 0); /* the last rail cell; z >= 8 is floor */
		clear_carts();
		struct block_data on, where;
		assert(server_world_get_block(&s.worlds[0], 6, RY, 6, &on));
		assert(server_world_get_block(&s.worlds[0], 6, RY + 1, 6, &where));
		struct block_info bon = {.block = &on, .x = 6, .y = RY, .z = 6};
		struct block_info bwhere = {.block = &where, .x = 6, .y = RY + 1, .z = 6};
		struct item_data held = {.id = ITEM_MINECART, .count = 1};
		assert(item_minecart.onItemPlace(&s, &s.player, &held, &bwhere, &bon,
										 SIDE_TOP));
		tick_once(); /* settle on the rail */
		struct entity* e = only_minecart();
		e->pos[2] = 6.5F;
		e->data.minecart.speed = 0.3F;
		e->data.minecart.hx = 0;
		e->data.minecart.hz = 1;
		float z_last = e->pos[2];
		bool left = false, moving_after = false, halved = false;
		float dz_prev = 0.0F;
		for(int t = 1; t <= 12; t++) {
			tick_once();
			e = only_minecart();
			float dz = e->pos[2] - z_last;
			z_last = e->pos[2];
			if(e->pos[2] >= 8.0F)
				left = true;
			if(left && dz > 0.0F)
				moving_after = true;
			/* once grounded off the rail, each tick's move is half the last
			 * (motion *= 0.5 before the move, no air drag) */
			if(left && e->on_ground && dz_prev > 1e-4F
			   && fabs((double)dz - 0.5 * (double)dz_prev) <= 1e-4)
				halved = true;
			dz_prev = dz;
		}
		CHECK(left, "(E) the cart rolls past the rail end (z %.4f)",
			  (double)e->pos[2]);
		CHECK(moving_after, "(E) it keeps moving off the rail (no freeze)");
		CHECK(halved, "(E) grounded off-rail travel halves per tick (onGround *0.5)");
		CHECK(near(e->pos[1], FLOOR + YOFF) && e->on_ground,
			  "(E) it settles on the floor: y %.6f onGround %d", (double)e->pos[1],
			  e->on_ground);
		printf("  (E) derail off an open rail end\n");
	}

	/* (F) Rail probe: Beta probes the feet cell and ONE BELOW only
	 * (:212-215). A rail one cell ABOVE the cart's feet does not make it
	 * on-rail: it moves under the off-rail rules (grounded halving). */
	{
		/* The cart rests at posY = FLOOR + 0.35 (feet cell RY); a rail in its
		 * OWN column one cell up (RY + 1) is exactly what the pre-G1 engine's
		 * +1 fallback found. Placed silently with no support block: the
		 * probe, not the rail's survival rule, is under test (a stone
		 * support at RY would be the cart's own cell). */
		set(10, RY + 1, 10, BLOCK_RAIL, 0);
		struct entity* e = offrail_cart(10.5F, (float)(FLOOR + YOFF), 10.2F, 0, 0, 0,
										false);
		tick_once();
		e = only_minecart();
		e->vel[2] = 0.2F;
		struct beta b = from_engine(e);
		beta_offrail_tick(&b, 0);
		tick_once();
		e = only_minecart();
		compare("(F) no upward rail probe", 1, e, &b);
		printf("  (F) rail probe never looks up\n");
	}

	/* (G) Off -> on: an off-rail cart whose motion is not on the rail axis
	 * reaches a straight NS rail. Beta projects the motion vector onto the
	 * rail axis keeping its horizontal MAGNITUDE, signed by the dot product
	 * with the MATRIX axis (+Z for shape 0) [:255-264]; posX snaps to the
	 * cell centre, the flat move advances Z by the magnitude (< 0.4, no
	 * clamp), unridden friction *0.96 follows [:345-347], posY = rail + 0.5,
	 * motionY = 0 and onGround false (moveEntity with dy = 0). Both signs. */
	{
		set(2, RY, 3, BLOCK_RAIL, 0);
		for(int sign = 1; sign >= -1; sign -= 2) {
			struct entity* e = offrail_cart(2.6F, (float)(FLOOR + YOFF), 3.5F,
											0.06F, 0.0F, 0.12F * (float)sign,
											true);
			const double m = sqrt((double)e->vel[0] * e->vel[0]
								  + (double)e->vel[2] * e->vel[2]);
			const double z0 = (double)e->pos[2];
			tick_once();
			e = only_minecart();
			const double want_speed = (double)sign * m * (double)0.96F;
			const double motion_z = e->data.minecart.speed
				* (double)e->data.minecart.hz;
			CHECK(e->data.minecart.hx == 0 && near(motion_z, want_speed),
				  "(G%+d) projected rail motion z %.6f (h %d,%d) Beta %.6f", sign,
				  motion_z, e->data.minecart.hx, e->data.minecart.hz, want_speed);
			CHECK(e->pos[0] == 2.5F && near(e->pos[2], z0 + sign * m)
					  && near(e->pos[1], (double)RY + 0.5),
				  "(G%+d) pos (%.6f,%.6f,%.6f) Beta (2.5,%.6f,%.6f)", sign,
				  (double)e->pos[0], (double)e->pos[1], (double)e->pos[2],
				  (double)RY + 0.5, z0 + sign * m);
			CHECK(e->vel[0] == 0.0F && e->vel[1] == 0.0F && e->vel[2] == 0.0F
					  && !e->on_ground,
				  "(G%+d) folded: vel (%.6f,%.6f,%.6f) onGround %d", sign,
				  (double)e->vel[0], (double)e->vel[1], (double)e->vel[2],
				  e->on_ground);
		}
		printf("  (G) off-rail motion projected onto a rail\n");
	}

	clear_carts();
	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	printf("%s test_minecart_offrail_golden (%d failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
