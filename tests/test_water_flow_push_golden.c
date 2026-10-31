/*
	test_water_flow_push_golden -- next-gaps-0924/W2 MOVE card 1 (P3).
	FAITHFULNESS golden (Beta 1.7.3, mcb173-retromcp src_original) for the
	water-flow push on players, mobs and dropped items, authored BEFORE the
	port it guards. Intent: docs/intent/p3-water-flow-push.md.

	Spec:
	  World.handleMaterialAcceleration:1418-1450
	      x0 = floor(minX) .. x1 = floor(maxX + 1) (half-open), same for y/z;
	      !checkChunksExist(x0,y0,z0,x1,y1,z1) -> false, no impulse (:1425);
	      every water cell of the span adds BlockFluid.velocityToAddToEntity
	      (:1428-1443; the `(double)var7 >= var16` surface test compares
	      against the exclusive top y1 and is true for every scanned cell);
	      lengthVector() > 0 -> normalize, motion += n * 0.014D (:1445-1449).
	  World.checkChunksExist:327-348   `if(var5 >= 0 && var2 < 128)`, then every
	      chunk of x0>>4 .. x1>>4, z0>>4 .. z1>>4 INCLUSIVE must exist -- the
	      span reaches floor(maxX+1)'s chunk, one past the last scanned cell.
	  BlockFluid.getFlowVector:86-166  neighbours -X, -Z, +X, +Z; water
	      neighbour: (its decay - decay); non-water non-solid neighbour: the
	      cell below, (its decay - (decay - 8)); raw meta >= 8 with a solid
	      side among the eight listed cells -> normalize + (0,-6,0); normalize.
	  BlockFluid.getEffectiveFlowDecay:35-45, getPercentAir:18-25,
	  getIsBlockSolid:60-63 (water/ice never solid), Block.getIsBlockSolid:
	  239-241 (Material.isSolid), Vec3D.normalize:63-66 + lengthVector:97-99
	  through MathHelper.sqrt_double:18-20 = (float)Math.sqrt.
	  Entity.handleWaterMovement:543-545  boundingBox.expand(0,-0.4F,0)
	      .func_28195_e(0.001,0.001,0.001) -- players and every EntityLiving.
	  EntityItem.handleWaterMovement:79-81  the PLAIN boundingBox.
	  Entity.onEntityUpdate:168-198  the call site: the FIRST thing a tick
	      does, before EntityLiving.onLivingUpdate's jump / moveFlying / move
	      (EntityLiving.java:583-641) and before EntityItem.onUpdate's gravity
	      (EntityItem.java:45); the dry->wet splash edge lives there too.
	  EntityLiving.moveEntityWithHeading:455-465  water branch: moveFlying
	      0.02F, moveEntity, motion *= 0.8F.  EntityItem.onUpdate:45-66: motionY
	      -= 0.04F, move, X/Z *= 0.98F airborne / slipperiness*0.98F grounded.

	HOW THE NUMBERS WERE MADE (CODESTYLE 4.2, poisoned-golden rule): a
	stand-alone transcription of the Java above,
	tmp/jobs/next-gaps-0924/W2M-p3-golden.py (Java float = binary32 via a
	struct round-trip, Java double = Python float), output in
	W2M-p3-golden.out. Nothing here was produced by running CaveWii. CaveWii
	stores motion/position as float (Beta: double) -- the pre-existing
	representation deviation (CODESTYLE 1.4) -- so sequences compare within
	VEL_TOL / POS_TOL; every RED delta on the pre-fix base is >= 1e-3 on the
	first tick (0.014 vs 0), three orders above them.
	Raw-Beta traceability (CODESTYLE 2.4): the impulse is Beta's 0.014D per
	tick along the unit flow vector; the diagonal 0.0098994951 is 0.014/sqrt2.

	Scenes: A straight +X stream (player: host, split-guest and MP-client
	walks -- one function, three sessions); B diagonal (Manhattan diamond);
	C still water (verdict true, impulse zero); D falling cell in a notch
	((1,-6,0) normalised: x pull 0.0023016, y pull -0.0138095) on a grounded
	player and an airborne item; E the 0.001 contraction at a cell edge
	(5.7F dry / 5.75F wet); F item in the stream, server tick AND the client
	prediction; G the chunk seam: an item whose floor(maxX+1) lands in an
	absent chunk is NOT in water (fire not quenched, no impulse); H mob in a
	3-deep stream; M a read-count measurement (100 mobs + 200 items, dry vs
	still water) for the Wii cost note.
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
#include "support/mob_motion_fixture.h"

#include "block/aabb.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/input_owner.h"
#include "platform/thread.h"
#include "world.h"

#define VEL_TOL 1e-6
#define POS_TOL 2e-5
#define Y_TOL 2e-4

static int s_checks, s_failures, s_section_start;
static const char* s_last_failed;

static void fail(const char* what, const char* msg) {
	s_failures++;
	if(s_last_failed != what)
		fprintf(stderr, "FAIL %s: %s\n", what, msg);
	s_last_failed = what;
}

static void near_or_die(const char* what, int tick, double got, double want,
						double tol) {
	s_checks++;
	if(fabs(got - want) > tol) {
		char msg[160];
		snprintf(msg, sizeof(msg),
				 "tick %d got %.10f want %.10f (|d|=%.3g > %.1g)", tick, got,
				 want, fabs(got - want), tol);
		fail(what, msg);
	}
}

static void check(const char* what, bool ok, const char* msg) {
	s_checks++;
	if(!ok)
		fail(what, msg);
}

static void section_done(const char* msg) {
	printf("%s %s\n", s_failures == s_section_start ? "PASS" : "RED ", msg);
	fflush(stdout);
	s_section_start = s_failures;
}

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* ------------------------------------------------------ Java-derived rows */

struct xv {
	double x, vx;
};
struct xvzv {
	double x, vx, z, vz;
};
struct xvyv {
	double x, vx, y, vy;
};

/* A / H: unit +X flow, push 0.014D, then *= 0.8F (player and mob alike). */
static const struct xv k_stream[] = {
	{5.5140000000, 0.0112000002}, {5.5392000002, 0.0201600004},
	{5.5733600006, 0.0273280008}, {5.6146880014, 0.0330624011},
	{5.6617504025, 0.0376499214}, {5.7134003239, 0.0413199378},
	{5.7687202617, 0.0442559509}, {5.8269762125, 0.0466047614},
};
/* B: diamond flow (2,0,2) -> (0.70710679, 0, 0.70710679), impulse
 * 0.009899495106034308 on both axes. */
static const struct xvzv k_diag[] = {
	{5.5098994951, 0.0079195962, 5.5098994951, 0.0079195962},
	{5.5277185864, 0.0142552733, 5.5277185864, 0.0142552733},
	{5.5518733548, 0.0193238150, 5.5518733548, 0.0193238150},
	{5.5810966649, 0.0233786484, 5.5810966649, 0.0233786484},
	{5.6143748084, 0.0266225152, 5.6143748084, 0.0266225152},
	{5.6508968187, 0.0292176087, 5.6508968187, 0.0292176087},
	{5.6900139225, 0.0312936835, 5.6900139225, 0.0312936835},
	{5.7312071011, 0.0329545434, 5.7312071011, 0.0329545434},
};
/* D: falling notch, flow (9,0,0) -> normalize + (0,-6,0) -> (1,-6,0)/6.0827627F
 * = (0.1643989822, -0.9863938934, 0); impulse (0.0023015858884,
 * -0.0138095153302, 0). Grounded player: x/vx (the Y pull dies on the floor). */
static const struct xv k_notch_player[] = {
	{5.5023015859, 0.0018412687},
	{5.5064444405, 0.0033142838},
	{5.5120603102, 0.0044926958},
	{5.5188545918, 0.0054354254},
};
/* D: airborne item in the same notch (posY 101.625, box 101.5..101.75): push,
 * motionY -= 0.04F, move, *= 0.98F on X and Y. */
static const struct xvyv k_notch_item[] = {
	{5.5023015859, 0.0022555542, 101.5711904856, -0.0527333252},
	{5.5068587260, 0.0044659974, 101.4646476460, -0.1044119848},
	{5.5136263093, 0.0066322317, 101.3064261467, -0.1550570723},
	/* the 4th tick would put the box bottom (100.97) through the stone top
	 * at 101.0 -- Beta clips that move (Entity.moveEntity), so the airborne
	 * recurrence ends here. */
};
/* F / G control: grounded item in the +X stream, friction 0.6F*0.98F. */
static const struct xv k_item_stream[] = {
	{5.5140000000, 0.0082320008}, {5.5362320008, 0.0130724178},
	{5.5633044186, 0.0159185833}, {5.5932230019, 0.0175921287},
	{5.6248151306, 0.0185761736}, {5.6573913042, 0.0191547920},
};
static const struct xv k_item_seam_control[] = {
	{62.5140000000, 0.0082320008},
	{62.5362320008, 0.0130724178},
	{62.5633044186, 0.0159185833},
};

/* ----------------------------------------------------------- client half */

static void cfill(int x0, int x1, int y0, int y1, int z0, int z1,
				  uint8_t type, uint8_t meta) {
	for(int x = x0; x <= x1; x++)
		for(int y = y0; y <= y1; y++)
			for(int z = z0; z <= z1; z++)
				world_set_block(&gstate.world, x, y, z,
								(struct block_data) {.type = type,
													 .metadata = meta},
								false);
}

/* Beta stream along +X at (y, z): still source at x_src, then flowing water
 * with meta x - x_src (BlockFlowing spreads +1 per block). Stone floor at
 * y-1, stone banks at z +- 1. */
static void cstream(int x_src, int x_end, int y, int z) {
	cfill(x_src - 2, x_end + 2, y - 1, y - 1, z - 1, z + 1, BLOCK_STONE, 0);
	cfill(x_src - 2, x_end + 2, y, y, z - 1, z - 1, BLOCK_STONE, 0);
	cfill(x_src - 2, x_end + 2, y, y, z + 1, z + 1, BLOCK_STONE, 0);
	for(int x = x_src; x <= x_end; x++)
		cfill(x, x, y, y, z, z, x == x_src ? BLOCK_WATER_STILL : BLOCK_WATER_FLOW,
			  (uint8_t)(x - x_src));
}

static struct client_session s_owner;

static void walker(struct entity* e, struct client_session* owner, float x,
				   float feet, float z, float vx) {
	memset(e, 0, sizeof(*e));
	entity_local_player(91, e, &gstate.world);
	e->pos[0] = x;
	e->pos[1] = feet + 1.62F;
	e->pos[2] = z;
	e->orient[0] = 0.0F;
	e->orient[1] = GLM_PI_2;
	e->vel[0] = vx;
	e->on_ground = true;
	e->data.local_player.capture_input = true;
	memset(owner, 0, sizeof(*owner));
	owner->local_player = e;
	owner->oxygen = MAX_OXYGEN;
}

static void step(struct entity* e, struct client_session* owner) {
	struct input_player_frame frame = {.connected = true};
	assert(!entity_local_player_walk_owned(e, owner, &frame));
}

static void run_stream_walk(const char* what, struct client_session* owner) {
	struct entity e;
	walker(&e, owner, 5.5F, 101.0F, 5.5F, 0.0F);
	for(int t = 0; t < N(k_stream); t++) {
		step(&e, owner);
		near_or_die(what, t, e.vel[0], k_stream[t].vx, VEL_TOL);
		near_or_die(what, t, e.pos[0], k_stream[t].x, POS_TOL);
		near_or_die(what, t, e.pos[2], 5.5, POS_TOL);
	}
}

static void test_player_straight(void) {
	cstream(2, 9, 101, 5);
	run_stream_walk("A host walk", &s_owner);
	/* The split guest runs the same walk on its own session (the same owner
	 * function, a second client_session); the MP client predicts the push
	 * like Beta's client (gstate.remote_session gates only the footstep). */
	static struct client_session guest;
	run_stream_walk("A guest walk", &guest);
	gstate.remote_session = true;
	run_stream_walk("A remote-client walk", &s_owner);
	gstate.remote_session = false;
	cfill(0, 11, 100, 101, 4, 6, BLOCK_AIR, 0);
	section_done("A player pushed 0.014D/tick along a straight stream");
}

static void test_player_diagonal(void) {
	/* Manhattan diamond: source (2,101,2), meta = |dx|+|dz| <= 7. */
	cfill(-7, 11, 100, 100, -7, 11, BLOCK_STONE, 0);
	for(int x = -7; x <= 11; x++)
		for(int z = -7; z <= 11; z++) {
			int d = abs(x - 2) + abs(z - 2);
			if(d <= 7)
				cfill(x, x, 101, 101, z, z,
					  d == 0 ? BLOCK_WATER_STILL : BLOCK_WATER_FLOW,
					  (uint8_t)d);
		}
	struct entity e;
	walker(&e, &s_owner, 5.5F, 101.0F, 5.5F, 0.0F);
	for(int t = 0; t < N(k_diag); t++) {
		step(&e, &s_owner);
		near_or_die("B diagonal vx", t, e.vel[0], k_diag[t].vx, VEL_TOL);
		near_or_die("B diagonal x", t, e.pos[0], k_diag[t].x, POS_TOL);
		near_or_die("B diagonal vz", t, e.vel[2], k_diag[t].vz, VEL_TOL);
		near_or_die("B diagonal z", t, e.pos[2], k_diag[t].z, POS_TOL);
	}
	cfill(-7, 11, 100, 101, -7, 11, BLOCK_AIR, 0);
	section_done("B player pushed along the diagonal of a spreading pool");
}

static void test_player_still(void) {
	cfill(1, 9, 100, 100, 1, 9, BLOCK_STONE, 0);
	cfill(2, 8, 101, 103, 2, 8, BLOCK_WATER_STILL, 0);
	struct entity e;
	walker(&e, &s_owner, 5.5F, 101.0F, 5.5F, 0.0F);
	for(int t = 0; t < 4; t++) {
		step(&e, &s_owner);
		near_or_die("C still vx", t, e.vel[0], 0.0, 0.0);
		near_or_die("C still vz", t, e.vel[2], 0.0, 0.0);
		near_or_die("C still x", t, e.pos[0], 5.5, 0.0);
	}
	/* inWater is still TRUE in still water: the water branch's drag applies
	 * (0.1F * 0.8F), not the ground friction. */
	walker(&e, &s_owner, 5.5F, 101.0F, 5.5F, 0.1F);
	step(&e, &s_owner);
	near_or_die("C still drag", 0, e.vel[0], 0.1 * 0.800000011920929, VEL_TOL);
	cfill(1, 9, 100, 103, 1, 9, BLOCK_AIR, 0);
	section_done("C still water: in water, zero impulse");
}

static void notch(void) {
	cfill(2, 9, 100, 100, 2, 9, BLOCK_STONE, 0);
	cfill(5, 5, 101, 101, 5, 5, BLOCK_WATER_FLOW, 8); /* falling cell */
	cfill(4, 4, 101, 101, 5, 5, BLOCK_STONE, 0);
	cfill(5, 5, 101, 101, 4, 4, BLOCK_STONE, 0);
	cfill(5, 5, 101, 101, 6, 6, BLOCK_STONE, 0);
	cfill(6, 6, 100, 100, 5, 5, BLOCK_WATER_FLOW, 1); /* under the +X air */
}

static void test_player_falling(void) {
	notch();
	struct entity e;
	walker(&e, &s_owner, 5.5F, 101.0F, 5.5F, 0.0F);
	for(int t = 0; t < N(k_notch_player); t++) {
		step(&e, &s_owner);
		near_or_die("D notch player vx", t, e.vel[0], k_notch_player[t].vx,
					VEL_TOL);
		near_or_die("D notch player x", t, e.pos[0], k_notch_player[t].x,
					POS_TOL);
	}
	/* The client item prediction in the same notch, airborne: the (0,-6,0)
	 * pull is visible on Y. */
	struct entity it;
	memset(&it, 0, sizeof it);
	entity_item(93, &it, false, &gstate.world,
				(struct item_data) {.id = BLOCK_COBBLESTONE, .count = 1});
	it.teleport(&it, (vec3) {5.5F, 101.625F, 5.5F});
	glm_vec3_zero(it.vel);
	for(int t = 0; t < N(k_notch_item); t++) {
		assert(!it.tick_client(&it));
		near_or_die("D notch item vx", t, it.vel[0], k_notch_item[t].vx, VEL_TOL);
		near_or_die("D notch item x", t, it.pos[0], k_notch_item[t].x, POS_TOL);
		near_or_die("D notch item vy", t, it.vel[1], k_notch_item[t].vy, VEL_TOL);
		near_or_die("D notch item y", t, it.pos[1], k_notch_item[t].y, Y_TOL);
	}
	cfill(2, 9, 100, 101, 2, 9, BLOCK_AIR, 0);
	section_done("D falling cell against a wall pulls (1,-6,0) normalised");
}

static void test_player_boundary(void) {
	/* Still water in column x=6 only; a player at 5.7F has maxX + 1 =
	 * 5.7F + 0.3F - 0.001 + 1 = 6.999 -> cells [5,6): dry (ground friction
	 * 0.546F on a 0.1F launch); at 5.75F the span reaches cell 6: wet
	 * (0.8F drag). */
	cfill(1, 9, 100, 100, 1, 9, BLOCK_STONE, 0);
	cfill(6, 6, 101, 101, 2, 8, BLOCK_WATER_STILL, 0);
	struct entity e;
	walker(&e, &s_owner, 5.7F, 101.0F, 5.5F, 0.1F);
	step(&e, &s_owner);
	near_or_die("E dry at 5.7F", 0, e.vel[0], 0.1 * 0.546000063419342, VEL_TOL);
	walker(&e, &s_owner, 5.75F, 101.0F, 5.5F, 0.1F);
	step(&e, &s_owner);
	near_or_die("E wet at 5.75F", 0, e.vel[0], 0.1 * 0.800000011920929, VEL_TOL);
	cfill(1, 9, 100, 101, 1, 9, BLOCK_AIR, 0);
	section_done("E the 0.001 contraction decides the cell edge");
}

/* ----------------------------------------------------------- server half */

static char s_fixture[128];
static struct server_local s;
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

static void sset(int x, int y, int z, int type, int meta) {
	struct block_data cur;
	if(server_world_get_block(&s.worlds[0], x, y, z, &cur) && cur.type == type
	   && cur.metadata == meta)
		return; /* a no-op write is refused */
	bool ok = server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type,
							 .metadata = (uint8_t)(meta & 0x0F)});
	assert(ok);
}

static void sfill(int x0, int x1, int y0, int y1, int z0, int z1, int type,
				  int meta) {
	for(int x = x0; x <= x1; x++)
		for(int y = y0; y <= y1; y++)
			for(int z = z0; z <= z1; z++)
				sset(x, y, z, type, meta);
}

static void sstream(int x_src, int x_end, int y0, int y1, int z) {
	sfill(x_src - 2, x_end + 2, y0 - 1, y0 - 1, z - 1, z + 1, BLOCK_STONE, 0);
	sfill(x_src - 2, x_end + 2, y0, y1, z - 1, z - 1, BLOCK_STONE, 0);
	sfill(x_src - 2, x_end + 2, y0, y1, z + 1, z + 1, BLOCK_STONE, 0);
	for(int x = x_src; x <= x_end; x++)
		sfill(x, x, y0, y1, z, z,
			  x == x_src ? BLOCK_WATER_STILL : BLOCK_WATER_FLOW, x - x_src);
}

static struct entity* make_item(float x, float y, float z) {
	struct entity* e = malloc(sizeof *e);
	assert(e);
	memset(e, 0, sizeof *e);
	entity_item(900, e, true, &s.worlds[0],
				(struct item_data) {.id = BLOCK_COBBLESTONE, .count = 1});
	e->teleport(e, (vec3) {x, y, z});
	glm_vec3_zero(e->vel);
	e->data.item.was_in_water = true; /* isFirstUpdate: no splash */
	return e;
}

static void test_item_stream(void) {
	sstream(2, 9, 101, 101, 20);
	struct entity* it = make_item(5.5F, 101.125F, 20.5F);
	for(int t = 0; t < N(k_item_stream); t++) {
		assert(!it->tick_server(it, &s));
		near_or_die("F server item vx", t, it->vel[0], k_item_stream[t].vx,
					VEL_TOL);
		near_or_die("F server item x", t, it->pos[0], k_item_stream[t].x,
					POS_TOL);
		near_or_die("F server item z", t, it->pos[2], 20.5, POS_TOL);
	}
	free(it);
	/* The client prediction of the same tick, in the client-world copy of the
	 * stream (the server sends no per-tick item moves, W1C-b). */
	cstream(2, 9, 101, 20);
	struct entity ci;
	memset(&ci, 0, sizeof ci);
	entity_item(94, &ci, false, &gstate.world,
				(struct item_data) {.id = BLOCK_COBBLESTONE, .count = 1});
	ci.teleport(&ci, (vec3) {5.5F, 101.125F, 20.5F});
	glm_vec3_zero(ci.vel);
	for(int t = 0; t < N(k_item_stream); t++) {
		assert(!ci.tick_client(&ci));
		near_or_die("F client item vx", t, ci.vel[0], k_item_stream[t].vx,
					VEL_TOL);
		near_or_die("F client item x", t, ci.pos[0], k_item_stream[t].x,
					POS_TOL);
	}
	cfill(0, 11, 100, 101, 19, 21, BLOCK_AIR, 0);
	section_done("F dropped item drifts with the stream (server + client prediction)");
}

static void test_item_seam(void) {
	/* Stream x=60..63 at z=40, y=101. Chunks cx 0..3 are loaded, cx 4 is not.
	 * control 62.5: span floor(62.625+1)=63 -> chunk 3 only -> in water.
	 * seam 63.4: cells [63,64) but floor(63.525+1)=64 -> chunk 4 -> false. */
	/* Built by hand: nothing may be written at x >= 64 (chunk 4 must stay
	 * absent), so the banks/floor stop at x=63 with the stream. */
	sfill(58, 63, 100, 100, 39, 41, BLOCK_STONE, 0);
	sfill(58, 63, 101, 101, 39, 39, BLOCK_STONE, 0);
	sfill(58, 63, 101, 101, 41, 41, BLOCK_STONE, 0);
	for(int x = 60; x <= 63; x++)
		sfill(x, x, 101, 101, 40, 40,
			  x == 60 ? BLOCK_WATER_STILL : BLOCK_WATER_FLOW, x - 60);
	struct entity* ctl = make_item(62.5F, 101.125F, 40.5F);
	struct entity* seam = make_item(63.4F, 101.125F, 40.5F);
	ctl->data.item.persisted_fire = 50;
	seam->data.item.persisted_fire = 50;
	for(int t = 0; t < N(k_item_seam_control); t++) {
		assert(!ctl->tick_server(ctl, &s));
		assert(!seam->tick_server(seam, &s));
		near_or_die("G control vx", t, ctl->vel[0], k_item_seam_control[t].vx,
					VEL_TOL);
		near_or_die("G control x", t, ctl->pos[0], k_item_seam_control[t].x,
					POS_TOL);
		near_or_die("G seam vx", t, seam->vel[0], 0.0, 0.0);
		near_or_die("G seam x", t, seam->pos[0], (double)63.4F, 0.0);
	}
	check("G control fire quenched", ctl->data.item.persisted_fire <= 0,
		  "fire should be 0 then -1 after the wet ticks");
	check("G seam fire NOT quenched", seam->data.item.persisted_fire == 47,
		  "not in water: fire only counted down 50 -> 47");
	check("G seam chunk still absent",
		  !server_world_chunk_resident(&s.worlds[0], 4, 2),
		  "the scan must not load chunk (4,2)");
	free(ctl);
	free(seam);
	section_done("G checkChunksExist spans floor(maxX+1): seam item is dry");
}

/* Settle a pig zombie on a dry stone platform by the production tick (the
 * W1B idiom), then slide it horizontally into the stream at the same floor. */
static struct entity* spawn_settled(float x, float z) {
	/* floor + 2, as W1B: the engine's 2.0/tick mob gravity moves 3.42 blocks on
	 * the second airborne tick and entity_intersection_threshold tests the
	 * destination box first, so a drop from higher up tunnels a 1-thick floor
	 * (pre-existing engine behaviour, outside this card). */
	vec3 pos = {x, 102.0F, z};
	struct entity* m = server_local_spawn_monster(pos, MONSTER_PIG_ZOMBIE, &s);
	assert(m && m->tick_server);
	float prev_y = m->pos[1];
	for(int i = 0; i < 120; i++) {
		m->vel[0] = 0.0f;
		m->vel[2] = 0.0f;
		m->pos[0] = x;
		m->pos[2] = z;
		test_mob_sync(m); /* motion units S2 */
		m->data.monster.path.length = 0;
		m->data.monster.knockback_time = 100;
		bool reaped = m->tick_server(m, &s);
		if(reaped || m->health <= 0) {
			printf("settle tick %d: reaped %d health %d pos %.3f %.3f %.3f "
				   "fire %d death_time %d delay_destroy %d\n",
				   i, (int)reaped, (int)m->health, m->pos[0], m->pos[1],
				   m->pos[2], m->data.monster.fire_time,
				   m->data.monster.death_time,
				   m->delay_destroy);
		}
		assert(!reaped && "mob must survive the settle");
		prev_y = m->pos[1];
	}
	assert(m->pos[1] == prev_y);
	assert(floorf(m->pos[1]) == 101.0F && m->pos[1] < 101.05F);
	m->data.monster.path.length = 0;
	m->data.monster.direction[0] = 0.0f;
	m->data.monster.direction[1] = 0.0f;
	m->vel[0] = m->vel[1] = m->vel[2] = 0.0f;
	test_mob_sync(m); /* motion units S2 */
	return m;
}

static void test_mob_stream(void) {
	/* 3-deep stream (y 101..103) at z=52 with the same +X gradient on every
	 * layer; dry settle platform at z 55..58. */
	sfill(0, 11, 100, 100, 50, 58, BLOCK_STONE, 0);
	sstream(2, 9, 101, 103, 52);
	struct entity* m = spawn_settled(5.5F, 56.5F);
	m->pos[2] = 52.5F;
	test_mob_sync(m); /* motion units S2 */
	m->data.monster.was_in_water = true;
	for(int t = 0; t < N(k_stream); t++) {
		m->data.monster.knockback_time = 100; /* no walk push, no Wii cap */
		bool reaped = m->tick_server(m, &s);
		assert(!reaped);
		near_or_die("H mob vx", t, m->vel[0], k_stream[t].vx, VEL_TOL);
		near_or_die("H mob x", t, m->pos[0], k_stream[t].x, POS_TOL);
		check("H mob stays in the stream", m->pos[1] >= 101.0F && m->pos[1] < 103.5F,
			  "mob left the 3-deep stream during the golden");
	}
	section_done("H mob pushed 0.014D/tick along the stream (pre-move, then 0.8F drag)");
}

/* Read-count measurement for the intent's Wii cost note: server_world
 * get_block reads per entity-tick, 100 pig zombies + 200 items, dry vs a
 * still 3-deep pool (the box scan alone is <= 8 cell reads; each water cell
 * adds 4 neighbour reads, up to 4 below-reads and, when falling, up to 8
 * side reads). Printed, not asserted: the number is platform-independent,
 * the time is Wii's. */
static void measure_reads(void) {
	sfill(-20, 20, 100, 100, 70, 111, BLOCK_STONE, 0);
	sfill(-20, 20, 101, 103, 70, 89, BLOCK_WATER_STILL, 0); /* pool */
	struct entity* mobs[100];
	int n = 0;
	for(int i = 0; i < 10; i++)
		for(int j = 0; j < 10; j++) {
			vec3 pos = {-18.5f + (float)i * 3.7f, 102.0f, 92.5f + (float)j * 1.9f};
			mobs[n] = server_local_spawn_monster(pos, MONSTER_PIG_ZOMBIE, &s);
			assert(mobs[n] && mobs[n]->tick_server);
			n++;
		}
	for(int t = 0; t < 40; t++)
		for(int k = 0; k < n; k++) {
			mobs[k]->data.monster.knockback_time = 100;
			mobs[k]->vel[0] = mobs[k]->vel[2] = 0.0f;
			test_mob_sync(mobs[k]); /* motion units S2 */
			assert(!mobs[k]->tick_server(mobs[k], &s));
		}
	struct entity* items[200];
	for(int k = 0; k < 200; k++)
		items[k] = make_item(-18.5f + (float)(k % 20) * 1.9f, 101.125f,
							 92.5f + (float)(k / 20) * 1.9f);
	/* The scan ALONE (the production ticks add pathfinding, hazard and
	 * pickup scans that would swamp the number): one handleWaterMovement per
	 * entity, counted through server_world's get_block counter. */
	int us, sn, smax, gn, gmax;
	int reads[4];
	for(int pass = 0; pass < 2; pass++) {
		if(pass == 1) { /* into the pool, same floor */
			for(int k = 0; k < n; k++) {
				mobs[k]->pos[2] -= 20.0f;
				test_mob_sync(mobs[k]); /* motion units S2 */
			}
			for(int k = 0; k < 200; k++)
				items[k]->pos[2] -= 20.0f;
		}
		server_world_perf_tick_reset();
		for(int k = 0; k < n; k++) {
			struct entity* m = mobs[k];
			bool wet = entity_handle_water_movement(
				m, (double)m->pos[0],
				(double)m->pos[1] - (double)m->y_offset + (double)m->y_size,
				(double)m->pos[2], m->width, m->height);
			check("M mob verdict", wet == (pass == 1), "mob wet verdict");
		}
		/* perf_tick_reset CLOSES the window; edit_stats reads the last
		 * closed one (server_world.c) -- reset before and after, like W1B. */
		server_world_perf_tick_reset();
		server_world_edit_stats(&us, &sn, &smax, &gn, &gmax);
		reads[pass * 2] = gn;
		for(int k = 0; k < 200; k++) {
			struct entity* it = items[k];
			double half = (double)(it->width / 2.0F);
			double min_y = (double)it->pos[1] - (double)it->y_offset
				+ (double)it->y_size;
			bool wet = entity_water_acceleration(
				it, (double)it->pos[0] - half, min_y, (double)it->pos[2] - half,
				(double)it->pos[0] + half, min_y + (double)it->height,
				(double)it->pos[2] + half);
			check("M item verdict", wet == (pass == 1), "item wet verdict");
		}
		server_world_perf_tick_reset();
		server_world_edit_stats(&us, &sn, &smax, &gn, &gmax);
		reads[pass * 2 + 1] = gn;
	}
	printf("MEASURE handleWaterMovement get_block reads per entity: mob dry "
		   "%.1f wet(still pool) %.1f | item dry %.1f wet %.1f (100 mobs, 200 "
		   "items; a full mob tick reads thousands more for pathfinding)\n",
		   reads[0] / 100.0, reads[2] / 100.0, reads[1] / 200.0,
		   reads[3] / 200.0);
	for(int k = 0; k < 200; k++)
		free(items[k]);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_water_flow_push_golden_tmp", s_fixture,
					 sizeof s_fixture);
	assert(clin_init());
	blocks_init();
	items_init();

	/* client world */
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);

	test_player_straight();
	test_player_diagonal();
	test_player_still();
	test_player_falling();
	test_player_boundary();

	/* server world */
	MKDIR(s_fixture);
	MKDIR(test_fixture_join(s_fixture, "/region"));
	remove(test_fixture_join(s_fixture, "/region/r.0.0.mcr"));
	string_t level;
	string_init_set_str(level, s_fixture);
	memset(&s, 0, sizeof(s));
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.worlds[0].local = &s;
	s.scheduled_tick_budget = 128;
	s.rand_src.seed = 0x51DEB002u;
	s.entity_rng.seed = 0x13579BDFu;
	dict_entity_init(s.entities);
	/* positionless player (no AI target), parked within 128 blocks of every
	 * mob scene so despawnEntity never fires; far from every item. */
	s.player.x = 8.5;
	s.player.y = 140.0;
	s.player.z = 70.0;

	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));

	/* cx 4 stays ABSENT on purpose (section G). Mobs live at x <= 20, so
	 * their +-32 chunk gate never reaches it. */
	struct server_chunk* sc = NULL;
	for(w_coord_t cx = -3; cx <= 3; cx++)
		for(w_coord_t cz = -3; cz <= 9; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);

	test_item_stream();
	test_item_seam();
	test_mob_stream();
	measure_reads();

	g_drain_run = 0;
	thread_join(&drain);
	if(s_failures) {
		fprintf(stderr, "test_water_flow_push_golden: %d of %d checks FAILED\n",
				s_failures, s_checks);
		return 1;
	}
	printf("PASS test_water_flow_push_golden (%d checks)\n", s_checks);
	return 0;
}
