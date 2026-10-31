/*
	Beta 1.7.3 minecart RAIL-TRAVEL COLLISION goldens (O06, KANBAN Beta-parity
	card: "minecart rail travel collision"). Authored BEFORE the fix, per the
	beta-tick-golden discipline; every expected value below is computed here
	from the decompile's own arithmetic, never guessed.

	GROUND TRUTH -- CFR server decompile
	/c/tmp/mcb173-server-decompile/out/net/minecraft/src/:
	  EntityMinecart.java:66-67   setSize(0.98F, 0.7F); yOffset = height / 2
	  EntityMinecart.java:289-297 the cart is projected onto the rail line, then
	                              setPosition(posX, posY + yOffset, posZ) with
	                              posY = y (flat) or y + 1 (slopes 2-5,
	                              :233-235) -> box minY = y (+1), height 0.7F
	  EntityMinecart.java:298-316 mx/mz = motionX/Z (*0.75 ridden),
	                              clamped to +-0.4, then
	                              moveEntity(mx, 0.0, mz)          (:316)
	  Entity.java:154-161         setPosition: half-width (double)(0.98F/2.0F),
	                              minY = posY - yOffset + ySize
	  Entity.java:257-...         moveEntity: getCollidingBoundingBoxes over
	                              boundingBox.addCoord(dx,dy,dz); clip Y, then X
	                              (calculateXOffset), then Z (calculateZOffset);
	                              field_9077_F = true (Entity.java:80) so a
	                              blocked axis does not zero the others;
	                              stepHeight 0 (Entity.java:93) -> no step;
	                              posX/Z = (box.min + box.max) / 2;
	                              motionX = 0 if X was clipped, motionZ = 0 if Z
	                              was clipped.
	  EntityMinecart.java:349-356 slope energy: s = |motion|; rescaled ONLY
	                              when s > 0.0 -> a stopped cart gains
	                              nothing from the height it is resting at.
	  EntityMinecart.java:375-389 powered at |motion| <= 0.01 on a flat straight:
	                              wall at z + 1 -> motionZ = -0.02.

	What the engine did instead: MOVE_ALONG_X/Z (entity_minecart.c) added the
	displacement straight to the position with no collision at all, so a cart
	ridden into a solid end stop ran INTO the stone cell, left the rail and was
	stuck forever (PC vehicle smoke vs_pc_cart_20261004_091553: cart at
	z = 3.079 inside the az+3 stop, every later ride window ride_cart_st1).

	ENGINE MAPPING. Positions are floats in this engine; Beta's are doubles.
	The collision result is computed with Beta's double box arithmetic and then
	rounded once, so where a clip decides the position the golden is EXACT
	(== (float)beta, or the adjacent float on the free side when rounding
	would put the re-derived box past Beta's face -- beta_face_f, O08).
	Unclipped travel keeps the engine's float add (asserted
	byte-identical to the pre-fix path). Speeds follow the existing goldens'
	1e-5 tolerance (test_minecart_rail_beta).

	NOT COVERED HERE (reported, separate cards -- see O06-result.md):
	  - World.getCollidingBoundingBoxes' ENTITY half (EntityMinecart.func_89_d
	    :77-79 returns every other entity's boundingBox) and the cart-cart
	    applyEntityCollision push (:441-447): this engine has neither;
	  - the OFF-RAIL branch (:385-406): this engine stops a cart that is not on
	    a rail instead of running Beta's derail moveEntity physics.
	  Both are printed as DIVERGENCE lines below, not asserted.
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
#define YF 100 /* stone floor */
#define RY 101 /* rail plane  */

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

static void set(int x, int y, int z, int type, int meta) {
	(void)server_world_set_block_silent(&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type,
							 .metadata = (uint8_t)(meta & 0x0F)});
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	assert(b.type == (uint8_t)type && (b.metadata & 0x0F) == (meta & 0x0F));
}

static bool place_cart_on(int x, int y, int z) {
	struct block_data blk_on, blk_where;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &blk_on));
	assert(server_world_get_block(&s.worlds[0], x, y + 1, z, &blk_where));
	struct block_info on = {
		.block = &blk_on, .neighbours = NULL, .x = x, .y = y, .z = z};
	struct block_info where = {
		.block = &blk_where, .neighbours = NULL, .x = x, .y = y + 1, .z = z};
	struct item_data held = {.id = ITEM_MINECART, .durability = 0, .count = 1};
	struct item* it = item_get(&held);
	assert(it == &item_minecart && it->onItemPlace);
	return it->onItemPlace(&s, &s.player, &held, &where, &on, SIDE_TOP);
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

/* A cart on the rail at (x, RY, z), settled, then given an exact state. */
static struct entity* cart_at(int x, int z, float pos_z, float speed,
							  int8_t hx, int8_t hz) {
	clear_carts();
	assert(place_cart_on(x, RY, z));
	assert(server_local_test_tick_entities(&s)); /* settle onto the rail */
	struct entity* e = only_minecart();
	e->pos[2] = pos_z;
	e->data.minecart.speed = speed;
	e->data.minecart.hx = hx;
	e->data.minecart.hz = hz;
	e->data.minecart.occupied = false;
	e->data.minecart.occupant_id = 0;
	e->data.minecart.pending_throttle = 0;
	return e;
}

#define CHECK(cond, ...) do { \
	if(!(cond)) { \
		fprintf(stderr, "FAIL: " __VA_ARGS__); \
		fprintf(stderr, "\n"); \
		failures++; \
	} \
} while(0)

/* Beta's numbers, in Beta's arithmetic. */
static const double HALF = (double)(0.98F / 2.0F); /* Entity.java:158 */

/* Entity.moveEntity's Z clip of the cart box against one full block whose
 * Z face is `face` (calculateZOffset, AxisAlignedBB.java), returning the new
 * posZ = (minZ + maxZ) / 2 after box.offset(0, 0, d). */
static double beta_clip_z(double posz, double d, double block_minz,
						  double block_maxz) {
	double minz = posz - HALF, maxz = posz + HALF;
	if(d > 0.0 && maxz <= block_minz) {
		double v = block_minz - maxz;
		if(v < d)
			d = v;
	} else if(d < 0.0 && minz >= block_maxz) {
		double v = block_maxz - minz;
		if(v > d)
			d = v;
	}
	minz += d;
	maxz += d;
	return (minz + maxz) / 2.0;
}

/* O08 / G4: the engine stores the clipped centre as a FLOAT and re-derives
 * the box every tick, while Beta keeps its double box. The engine takes the
 * float nearest Beta's centre whose re-derived box stays on Beta's side of
 * the clipped face (dir = the move's sign), so a cart resting at the face
 * still clips the next push into it, as Beta's box does. Exact
 * expectation: (float)beta, or the next float toward the free side when
 * (float)beta would put the box past Beta's face. */
static float beta_face_f(double posz, double d, double block_minz,
						 double block_maxz) {
	double c = beta_clip_z(posz, d, block_minz, block_maxz);
	double minz = c - HALF, maxz = c + HALF;
	float f = (float)c;
	if(d > 0.0) {
		while((double)f + HALF > maxz)
			f = nextafterf(f, -INFINITY);
	} else {
		while((double)f - HALF < minz)
			f = nextafterf(f, INFINITY);
	}
	return f;
}

int main(void) {
	setvbuf(stderr, NULL, _IONBF, 0);
	test_fixture_dir("test_minecart_rail_collision_golden_tmp",
					 s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
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
	for(int x = 0; x < 16; x++)
		for(int z = 0; z < 16; z++)
			set(x, YF, z, BLOCK_STONE, 0);

	/* (A) THE SMOKE GEOMETRY: straight NS rail, solid stone end stop.
	 * Rail (2,RY,4), stop (2,RY,5). Empty cart at z 4.5, +0.3 along +Z.
	 * Beta: var37 = 0.3, box maxZ 4.99 <= stop minZ 5 -> clipped to 0.01;
	 * motionZ = 0. Friction, slope energy, powered: all zero after that. */
	{
		set(2, RY, 4, BLOCK_RAIL, 0);
		set(2, RY, 5, BLOCK_STONE, 0);
		struct entity* e = cart_at(2, 4, 4.5F, 0.3F, 0, 1);
		assert(server_local_test_tick_entities(&s));
		e = only_minecart();
		float want = beta_face_f(4.5, 0.3, 5.0, 6.0);
		CHECK(e->pos[2] == want, "(A) end stop: z %.9g, Beta %.9g",
			  (double)e->pos[2], (double)want);
		CHECK(e->data.minecart.speed == 0.0F,
			  "(A) end stop: Beta zeroes motionZ (Entity.java moveEntity), "
			  "got speed %.9g", (double)e->data.minecart.speed);
		CHECK(e->pos[0] == 2.5F, "(A) end stop: X stays on the rail line");
		assert(server_local_test_tick_entities(&s));
		e = only_minecart();
		CHECK(e->pos[2] == want && e->data.minecart.speed == 0.0F,
			  "(A) end stop: the stopped cart stays on its rail (z %.9g)",
			  (double)e->pos[2]);
		CHECK((int)floorf(e->pos[2]) == 4,
			  "(A) end stop: the cart never enters the stone cell");
		/* (A3, O08) a push from REST at the face (a dismounted rider stepping
		 * off toward the open side): Beta's box clips it to 0 again. */
		e->data.minecart.speed = 0.05F;
		assert(server_local_test_tick_entities(&s));
		e = only_minecart();
		CHECK(e->pos[2] == want && e->data.minecart.speed == 0.0F,
			  "(A3) a push from rest at the stop face is clipped (z %.9g, "
			  "speed %.9g)", (double)e->pos[2],
			  (double)e->data.minecart.speed);
		printf("  (A) straight rail into a solid end stop\n");
	}

	/* (A2) A RIDDEN cart rolls into the stop over several ticks (rails at
	 * z 2,3; stop at z 4): the 0.75 displacement factor and the ridden
	 * friction 0.997F, then the exact clip. */
	{
		set(6, RY, 2, BLOCK_RAIL, 0);
		set(6, RY, 3, BLOCK_RAIL, 0);
		set(6, RY, 4, BLOCK_STONE, 0);
		struct entity* e = cart_at(6, 2, 2.5F, 0.3F, 0, 1);
		e->data.minecart.occupied = true;
		e->data.minecart.occupant_id = 77;
		double bz = 2.5, bv = 0.3;
		bool clipped = false;
		for(int t = 0; t < 12; t++) {
			assert(server_local_test_tick_entities(&s));
			e = only_minecart();
			if(!clipped) {
				double d = bv * 0.75;
				if(d > 0.4) d = 0.4;
				clipped = bz + HALF + d > 4.0; /* the box reaches the stop */
				bz = clipped ? beta_clip_z(bz, d, 4.0, 5.0) : bz + d;
				bv = clipped ? 0.0 : bv * (double)0.997F;
			}
			CHECK(fabs((double)e->pos[2] - bz) <= 2e-6,
				  "(A2) tick %d: z %.9g, Beta %.9g", t, (double)e->pos[2], bz);
			CHECK(fabs((double)e->data.minecart.speed - bv) <= 1e-5,
				  "(A2) tick %d: speed %.9g, Beta %.9g", t,
				  (double)e->data.minecart.speed, bv);
		}
		CHECK(clipped && e->pos[2] == (float)(4.0 - HALF),
			  "(A2) the ridden cart rests exactly at the stop face (z %.9g)",
			  (double)e->pos[2]);
		printf("  (A2) ridden multi-tick approach to the end stop\n");
	}

	/* (B) Side walls at x +-1 and a block over the rail at RY+1: Beta's box
	 * is X [x+0.01, x+0.99] (strict intersectsWith) and Y [RY, RY+0.7], so
	 * none of them clips -- the unobstructed float add is unchanged. */
	{
		set(10, RY, 2, BLOCK_RAIL, 0);
		set(10, RY, 3, BLOCK_RAIL, 0);
		set(9, RY, 2, BLOCK_STONE, 0);
		set(11, RY, 2, BLOCK_STONE, 0);
		set(10, RY + 1, 3, BLOCK_STONE, 0);
		struct entity* e = cart_at(10, 2, 2.5F, 0.3F, 0, 1);
		assert(server_local_test_tick_entities(&s));
		e = only_minecart();
		CHECK(e->pos[2] == 2.5F + 0.3F,
			  "(B) walls/headroom: unclipped travel is the pre-fix float add "
			  "(z %.9g)", (double)e->pos[2]);
		CHECK(fabsf(e->data.minecart.speed - 0.3F * 0.9599999785423279F) <= 1e-6F,
			  "(B) walls/headroom: only friction applied");
		printf("  (B) side walls + headroom never clip\n");
	}

	/* (C) POWERED rail at the stop: clip -> motion 0 -> the powered branch
	 * finds |motion| <= 0.01 on a flat NS straight with the wall at var3+1
	 * (:375-389) -> motionZ = -0.02: the cart is launched back. */
	{
		set(13, RY, 4, BLOCK_POWERED_RAIL, 0x8 | 0);
		set(13, RY, 5, BLOCK_STONE, 0);
		struct entity* e = cart_at(13, 4, 4.5F, 0.3F, 0, 1);
		assert(server_local_test_tick_entities(&s));
		e = only_minecart();
		float want = beta_face_f(4.5, 0.3, 5.0, 6.0);
		CHECK(e->pos[2] == want, "(C) powered stop: z %.9g, Beta %.9g",
			  (double)e->pos[2], (double)want);
		CHECK(fabsf(e->data.minecart.speed * (float)e->data.minecart.hz
					- (-0.02F)) <= 1e-6F,
			  "(C) powered stop: Beta's wall launch motionZ = -0.02 (got "
			  "speed %.9g hz %d)", (double)e->data.minecart.speed,
			  e->data.minecart.hz);
		printf("  (C) powered rail at the stop launches back at -0.02\n");
	}

	/* (D) SLOPE into a wall at its top: slope 4 rises toward -Z; Beta's
	 * move box is one block up (posY = var2 + 1, :233-235) -> Y [RY+1,
	 * RY+1.7], so the block at (x, RY+1, z-1) clips the -Z move at
	 * z = cell + HALF. The stopped cart then gains NO slope energy
	 * (:349-356 `if (var23 > 0.0)`). */
	{
		set(2, RY, 10, BLOCK_RAIL, 4);
		set(2, RY + 1, 9, BLOCK_STONE, 0);
		struct entity* e = cart_at(2, 10, 10.5F, -0.3F, 0, 1);
		assert(server_local_test_tick_entities(&s));
		e = only_minecart();
		/* slope impulse +1/128 on Z first (:244-246), then the move */
		double d = -0.3 + 0.0078125;
		float want = beta_face_f(10.5, d, 9.0, 10.0);
		CHECK(e->pos[2] == want, "(D) slope wall: z %.9g, Beta %.9g",
			  (double)e->pos[2], (double)want);
		CHECK(e->data.minecart.speed == 0.0F,
			  "(D) slope wall: motion zeroed and NO slope energy added "
			  "(got speed %.9g)", (double)e->data.minecart.speed);
		printf("  (D) slope into a wall at its top\n");
	}

	/* (E) Unobstructed straight + slope travel stays byte-identical to the
	 * pre-fix float path (the existing rail/slope/curve goldens pin the rest). */
	{
		set(14, RY, 10, BLOCK_RAIL, 0);
		set(14, RY, 11, BLOCK_RAIL, 0);
		struct entity* e = cart_at(14, 10, 10.5F, 0.3F, 0, 1);
		assert(server_local_test_tick_entities(&s));
		e = only_minecart();
		CHECK(e->pos[2] == 10.5F + 0.3F, "(E) open rail: z %.9g",
			  (double)e->pos[2]);
		printf("  (E) open rail unchanged\n");
	}

	/* DIVERGENCES this card does not change (printed, not asserted). */
	{
		set(6, RY, 10, BLOCK_RAIL, 0); /* a rail END, no stop beyond */
		struct entity* e = cart_at(6, 10, 10.5F, 0.3F, 0, 1);
		for(int t = 0; t < 3; t++)
			assert(server_local_test_tick_entities(&s));
		e = only_minecart();
		printf("  DIVERGENCE (not asserted, separate card): rail end with no "
			   "stop -- engine z %.4f speed %.4f; Beta runs the off-rail "
			   "branch (EntityMinecart.java:385-406: clamp 0.4, onGround *0.5, "
			   "moveEntity(motion), *0.95 airborne)\n",
			   (double)e->pos[2], (double)e->data.minecart.speed);
		printf("  DIVERGENCE (not asserted, separate card): cart-vs-entity "
			   "boxes (EntityMinecart.func_89_d :77-79 via "
			   "World.getCollidingBoundingBoxes) and cart-cart "
			   "applyEntityCollision (:441-447) are not implemented\n");
	}

	clear_carts();
	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	printf("%s test_minecart_rail_collision_golden (%d failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
