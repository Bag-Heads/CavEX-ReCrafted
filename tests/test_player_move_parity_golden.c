/*
	test_player_move_parity_golden -- next-gaps-0924/W1A. FAITHFULNESS golden
	(Beta 1.7.3, retromcp decompile of the b1.7.3 client) for four player
	movement/hazard gaps, authored BEFORE the port it guards.

	P1  SUFFOCATION. EntityLiving.onEntityUpdate:113-115
	        if(isEntityAlive() && isEntityInsideOpaqueBlock()) attackEntityFrom(null, 1);
	    Entity.isEntityInsideOpaqueBlock:851-863 -- 8 samples around the eye:
	        dx = ((float)((i >> 0) % 2) - 0.5F) * width * 0.9F
	        dy = ((float)((i >> 1) % 2) - 0.5F) * 0.1F
	        dz = ((float)((i >> 2) % 2) - 0.5F) * width * 0.9F
	        floor(posX + dx), floor(posY + getEyeHeight() + dy), floor(posZ + dz)
	        -> World.isBlockNormalCube:1636-1639
	    EntityPlayer.getEyeHeight:349-351 = 0.12F, width 0.6F (setSize), and the
	    SP player's posY is feet + yOffset 1.62F (EntityPlayer.java:44/151), i.e.
	    CaveWii's eye-basis server_player.y. The damage runs through
	    EntityLiving.attackEntityFrom:307-331 (heartsHalvesLife 20, a repeat
	    lands only when heartsLife <= 10 and exceeds the last hit), so a
	    1-point hazard lands at ticks 0, 10, 20, ... Player scale is x8
	    (1 Beta point = HEALTH_PER_HEART / 2 = 8).
	P2  GROUND FRICTION. EntityLiving.moveEntityWithHeading:478-497
	        f = 0.91F; if(onGround) { f = 546.0F * 0.1F * 0.1F * 0.1F;
	          id = getBlockId(floor(posX), floor(bb.minY) - 1, floor(posZ));
	          if(id > 0) f = Block.blocksList[id].slipperiness * 0.91F; }
	        moveFlying(s, f', onGround ? 0.1F * (0.16277136F / (f*f*f)) : 0.02F)
	        ... motionX *= f; motionZ *= f
	    Block.java:143 slipperiness 0.6F, BlockIce.java:8 0.98F.
	P6  LADDER. EntityLiving.moveEntityWithHeading:500-527: BEFORE moveEntity,
	    clamp motionX/Z to +-0.15F, motionY >= -0.15D, and
	        if(isSneaking() && motionY < 0.0D) motionY = 0.0D;   (:522-524)
	    then after the move `if(isCollidedHorizontally && isOnLadder())
	    motionY = 0.2D;` (:528-530) and gravity/drag (:532-535).
	R2  AIR / FLUID ACCEL 0.02F. moveEntityWithHeading:457 (water) and :469
	    (lava) call moveFlying(..., 0.02F); :489 uses 0.02F in air.
	Input: EntityLiving.onLivingUpdate:638-640 scales moveStrafing/moveForward
	by 0.98F right before moveEntityWithHeading, and Entity.moveFlying:570-585
	normalises by max(sqrt(s^2+f^2), 1) with the 0.01F dead zone. Every
	golden below includes that 0.98F.

	HOW THE NUMBERS WERE MADE (CODESTYLE 4.2, poisoned-golden rule): a
	stand-alone transcription of the Java above, Java float = IEEE binary32,
	Java double = binary64 (numpy float32 / Python float), yaw 0 so
	MathHelper.sin/cos are SIN_TABLE[0] = 0.0F and SIN_TABLE[16384] = 1.0F.
	Nothing here was produced by running CaveWii. CaveWii stores motion and
	position as float (Beta: double) -- a pre-existing engine representation,
	not part of this slice -- so sequences compare within VEL_TOL / POS_TOL;
	every RED delta on the pre-fix base is >= 1e-3, two orders above them.
	Raw-Beta traceability (CODESTYLE 2.4): the suffocation hit is 1 Beta point.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "support/split_residency_fixture.h"

#include "block/aabb.h"
#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/entity.h"
#include "game/game_state.h"
#include "item/inventory.h"
#include "item/items.h"
#include "network/inventory_logic.h"
#include "network/level_archive.h"
#include "platform/input_owner.h"
#include "world.h"

#define VEL_TOL 1e-6
#define POS_TOL 2e-5
#define Y_TOL 2e-4

static int s_checks, s_failures, s_section_start;
static const char* s_last_failed; /* report the FIRST miss of each golden */

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
		snprintf(msg, sizeof(msg), "tick %d got %.9f want %.9f (|d|=%.3g > %.1g)",
				 tick, got, want, fabs(got - want), tol);
		fail(what, msg);
	}
}

/* Every section runs even after an earlier one fails, so RED shows each gap. */
static void section_done(const char* msg) {
	printf("%s %s\n", s_failures == s_section_start ? "PASS" : "RED ", msg);
	fflush(stdout);
	s_section_start = s_failures;
}

/* ------------------------------------------------------------------ client */

static void fill(int x0, int x1, int y0, int y1, int z0, int z1, uint8_t type,
				 uint8_t meta) {
	for(int x = x0; x <= x1; x++)
		for(int y = y0; y <= y1; y++)
			for(int z = z0; z <= z1; z++)
				world_set_block(&gstate.world, x, y, z,
								(struct block_data) {.type = type,
													 .metadata = meta},
								false);
}

static struct client_session s_owner;

static void walker(struct entity* e, float x, float feet, float z,
				   bool on_ground, float vy) {
	memset(e, 0, sizeof(*e));
	entity_local_player(91, e, &gstate.world);
	e->pos[0] = x;
	e->pos[1] = feet + 1.62F;
	e->pos[2] = z;
	e->orient[0] = 0.0F; /* yaw 0: +forward is +Z, sin 0 / cos 1 exactly */
	e->orient[1] = GLM_PI_2;
	e->vel[1] = vy;
	e->on_ground = on_ground;
	e->data.local_player.capture_input = true;
	memset(&s_owner, 0, sizeof(s_owner));
	s_owner.local_player = e;
	s_owner.oxygen = MAX_OXYGEN;
}

static void step(struct entity* e, bool forward, bool sneak) {
	struct input_player_frame frame = {.connected = true};
	frame.down[IB_FORWARD] = forward;
	frame.down[IB_SNEAK] = sneak;
	assert(!entity_local_player_walk_owned(e, &s_owner, &frame));
}

/* Stand the walker on its floor with the engine's own landing, then zero the
 * horizontal state so the golden starts from Beta's rest (onGround, motion 0). */
static void settle_on_floor(struct entity* e) {
	for(int t = 0; t < 4; t++)
		step(e, false, false);
	assert(e->on_ground);
	e->pos[2] = 5.5F;
	e->vel[0] = e->vel[2] = 0.0F;
}

struct zv {
	double z, vz;
};

static void run_zv(const char* what, struct entity* e, bool forward,
				   const struct zv* want, int n) {
	for(int t = 0; t < n; t++) {
		step(e, forward, false);
		near_or_die(what, t, e->vel[2], want[t].vz, VEL_TOL);
		near_or_die(what, t, e->pos[2], want[t].z, POS_TOL);
	}
}

/* P2 + input 0.98F: stone floor, forward held. f = 0.6F*0.91F = 0.54600006F,
 * accel = 0.1F * (0.16277136F / f^3) = 0.09999999F. */
static const struct zv k_stone_walk[] = {
	{5.597999990, 0.053508001}, {5.749507981, 0.082723373},
	{5.930231343, 0.098674967}, {6.126906301, 0.107384539},
	{6.332290830, 0.112139966}, {6.542430786, 0.114736429},
	{6.755167205, 0.116154098}, {6.969321293, 0.116928146},
};
/* P2 ice: f = 0.98F*0.91F = 0.89180005F, accel = 0.022949621F. */
static const struct zv k_ice_walk[] = {
	{5.522490630, 0.020057145}, {5.565038405, 0.037944107},
	{5.625473142, 0.053895702}, {5.701859474, 0.068121334},
	{5.792471437, 0.080807754}, {5.895769821, 0.092121503},
	{6.010381954, 0.102211106}, {6.135083689, 0.111209013},
};
/* P2 ice, no input, launched at motionZ 0.3: pure friction decay. */
static const struct zv k_ice_slide[] = {
	{5.800000000, 0.267540014}, {6.067540014, 0.238592197},
	{6.306132210, 0.212776532}, {6.518908742, 0.189754121},
	{6.708662863, 0.169222734}, {6.877885597, 0.150912842},
	{7.028798439, 0.134584079}, {7.163382518, 0.120022088},
};
/* P2, block under the centre is AIR (id 0) while the body box is supported by
 * the neighbour: f stays 546.0F*0.1F*0.1F*0.1F (bit-equal to 0.6F*0.91F). */
static const struct zv k_edge_walk[] = {
	{5.597999990, 0.053508001}, {5.749507981, 0.082723373},
	{5.930231343, 0.098674967}, {6.126906301, 0.107384539},
};
/* R2 water: moveFlying 0.02F, then motionZ *= (double)0.8F. */
static const struct zv k_water[] = {
	{5.519600000, 0.015680000}, {5.554880001, 0.028224001},
	{5.602704002, 0.038259202}, {5.660563204, 0.046287362},
	{5.726450566, 0.052709891}, {5.798760457, 0.057847914},
	{5.876208371, 0.061958332}, {5.957766703, 0.065246667},
};
/* R2 lava: moveFlying 0.02F, then motionZ *= 0.5D. */
static const struct zv k_lava[] = {
	{5.519600000, 0.009800000}, {5.549000001, 0.014700000},
	{5.583300001, 0.017150000}, {5.620050001, 0.018375000},
	{5.658025002, 0.018987500}, {5.696612502, 0.019293750},
	{5.735506253, 0.019446875}, {5.774553128, 0.019523438},
};
/* R2 air: moveFlying 0.02F, motionZ *= 0.91F; y/motionY: -0.08D then *0.98F. */
static const struct {
	double z, vz, y, vy;
} k_air[] = {
	{5.519600000, 0.017836001, 110.620000000, -0.078400002},
	{5.557036001, 0.034066762, 110.541599998, -0.155232005},
	{5.610702763, 0.048836755, 110.386367994, -0.230527369},
	{5.679139518, 0.062277449, 110.155840625, -0.304316827},
	{5.761016968, 0.074508481, 109.851523798, -0.376630498},
	{5.855125449, 0.085638720, 109.474893299, -0.447497897},
	{5.960364169, 0.095767238, 109.027395402, -0.516947949},
	{6.075731408, 0.104984190, 108.510447453, -0.585009002},
};
/* P6: on a ladder, no input. Sneaking HOLDS (motionY < 0 -> 0 before the
 * move); plain descends at the pre-move -0.15D clamp. Eye-basis y. */
static const double k_ladder_sneak_y = 106.62;
static const double k_ladder_sneak_vy = -0.078400002;
static const struct {
	double y, vy;
} k_ladder_plain[] = {
	{106.620000000, -0.078400002}, {106.541599998, -0.155232005},
	{106.391599998, -0.225400004}, {106.241599998, -0.225400004},
	{106.091599998, -0.225400004}, {105.941599998, -0.225400004},
	{105.791599998, -0.225400004}, {105.641599998, -0.225400004},
};

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void test_ground_friction(void) {
	struct entity e;
	/* Stone floor at y=100, a stone course under it at 99. */
	fill(2, 8, 99, 100, 2, 14, BLOCK_STONE, 0);
	walker(&e, 5.5F, 101.0F, 5.5F, true, -0.0784F);
	settle_on_floor(&e);
	run_zv("P2 stone walk", &e, true, k_stone_walk, N(k_stone_walk));

	fill(2, 8, 100, 100, 2, 14, BLOCK_ICE, 0);
	walker(&e, 5.5F, 101.0F, 5.5F, true, -0.0784F);
	settle_on_floor(&e);
	run_zv("P2 ice walk", &e, true, k_ice_walk, N(k_ice_walk));

	walker(&e, 5.5F, 101.0F, 5.5F, true, -0.0784F);
	settle_on_floor(&e);
	e.vel[2] = 0.3F;
	run_zv("P2 ice slide", &e, false, k_ice_slide, N(k_ice_slide));
	fill(2, 8, 99, 100, 2, 14, BLOCK_AIR, 0);

	/* Edge: only the x=4 course exists; the centre cell (5, 100) is air. */
	fill(4, 4, 100, 100, 2, 14, BLOCK_STONE, 0);
	walker(&e, 5.2F, 101.0F, 5.5F, true, -0.0784F);
	settle_on_floor(&e);
	run_zv("P2 air-under edge walk", &e, true, k_edge_walk, N(k_edge_walk));
	fill(4, 4, 100, 100, 2, 14, BLOCK_AIR, 0);
	section_done("P2 ground friction (stone, ice walk, ice slide, id-0 edge)");
}

static void test_fluid_and_air_accel(void) {
	struct entity e;
	fill(2, 8, 100, 100, 2, 14, BLOCK_STONE, 0);
	fill(2, 8, 101, 106, 2, 14, BLOCK_WATER_STILL, 0);
	walker(&e, 5.5F, 102.0F, 5.5F, false, 0.0F);
	run_zv("R2 water", &e, true, k_water, N(k_water));

	fill(2, 8, 101, 106, 2, 14, BLOCK_LAVA_STILL, 0);
	walker(&e, 5.5F, 102.0F, 5.5F, false, 0.0F);
	run_zv("R2 lava", &e, true, k_lava, N(k_lava));
	fill(2, 8, 100, 106, 2, 14, BLOCK_AIR, 0);

	walker(&e, 5.5F, 109.0F, 5.5F, false, 0.0F);
	for(int t = 0; t < N(k_air); t++) {
		step(&e, true, false);
		near_or_die("R2 air vz", t, e.vel[2], k_air[t].vz, VEL_TOL);
		near_or_die("R2 air z", t, e.pos[2], k_air[t].z, POS_TOL);
		near_or_die("R2 air vy", t, e.vel[1], k_air[t].vy, VEL_TOL);
		near_or_die("R2 air y", t, e.pos[1], k_air[t].y, Y_TOL);
	}
	section_done("R2 0.02F accel in water, lava and air");
}

static void test_ladder(void) {
	struct entity e;
	/* A ladder column facing -Z (meta 2: box against the z+1 wall), well
	 * clear of the walker's 0.6 body at the cell centre. */
	fill(30, 30, 98, 108, 5, 5, BLOCK_LADDER, 2);
	fill(30, 30, 98, 108, 6, 6, BLOCK_STONE, 0);

	walker(&e, 30.5F, 105.0F, 5.5F, false, 0.0F);
	for(int t = 0; t < 12; t++) {
		step(&e, false, true);
		near_or_die("P6 ladder sneak y", t, e.pos[1], k_ladder_sneak_y, Y_TOL);
		near_or_die("P6 ladder sneak vy", t, e.vel[1], k_ladder_sneak_vy,
					VEL_TOL);
	}

	walker(&e, 30.5F, 105.0F, 5.5F, false, 0.0F);
	for(int t = 0; t < N(k_ladder_plain); t++) {
		step(&e, false, false);
		near_or_die("P6 ladder plain y", t, e.pos[1], k_ladder_plain[t].y,
					Y_TOL);
		near_or_die("P6 ladder plain vy", t, e.vel[1], k_ladder_plain[t].vy,
					VEL_TOL);
	}
	fill(30, 30, 98, 108, 5, 6, BLOCK_AIR, 0);
	section_done("P6 ladder: sneak holds, plain descent clamps pre-move");
}

/* The removed client snap-up (85feb7ba) is not Beta: Beta has no lift, and
 * AxisAlignedBB.calculate{X,Y,Z}Offset never clip against a box the mover is
 * already inside, so an embedded player walks out on its own (CaveWii's
 * entity_intersects_new_boxes rule, 3e3e3c18). Recovery case: a player whose
 * whole body is inside a stone column must NOT be lifted, and must walk out. */
static void test_embedded_recovery(void) {
	struct entity e;
	fill(40, 40, 100, 100, 3, 9, BLOCK_STONE, 0);
	fill(40, 40, 101, 102, 5, 5, BLOCK_STONE, 0);
	walker(&e, 40.5F, 101.0F, 5.5F, true, -0.0784F);
	const float y0 = e.pos[1];
	step(&e, false, false);
	near_or_die("embedded idle: no snap-up lift", 0, e.pos[1], y0, 0.02);
	int t = 0;
	for(; t < 40; t++) {
		step(&e, true, false);
		if(!(e.pos[1] < y0 + 0.02F))
			fail("embedded walk-out", "lifted while walking out (snap-up)");
		struct AABB box;
		aabb_setsize_centered(&box, 0.6F, 1.8F, 0.6F);
		aabb_translate(&box, e.pos[0], e.pos[1] + 0.9F - 1.62F, e.pos[2]);
		if(e.pos[2] > 6.31F && !entity_aabb_intersection(&e, &box))
			break;
	}
	if(t >= 40)
		fail("embedded walk-out", "never walked out of the block");
	fill(40, 40, 100, 102, 3, 9, BLOCK_AIR, 0);
	printf("  embedded walk-out took %d ticks\n", t + 1);
	section_done("embedded player: no lift, walks out");
}

/* ------------------------------------------------------------------ server */

/* The damage path emits health/sound RPCs mid-update; a concurrent drain keeps
 * clin_rpc_send's token backpressure from wedging the synchronous tick (the
 * test_cactus_contact_damage harness shape). */
static volatile int g_drain_run;
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void settle_server(struct server_local* s, unsigned n) {
	for(unsigned t = 0; t < n; t++)
		server_local_test_update_once(s);
}

static void put(struct server_local* s, int x, int y, int z, uint8_t type) {
	struct block_data b = {.type = type};
	(void)server_world_set_block(&s->worlds[0], x, y, z, b);
	struct block_data live;
	assert(server_world_get_block(&s->world, x, y, z, &live)
		   && live.type == type);
}

static void reset_body(struct server_player* p, double x, double feet,
					   double z) {
	p->x = x;
	p->y = feet + (double)PLAYER_EYE_HEIGHT;
	p->z = z;
	p->health = MAX_PLAYER_HEALTH;
	p->hurt_resist_time = 0;
	p->last_damage = 0;
	p->damage_remainder = 0;
	p->oxygen = MAX_OXYGEN;
	p->fire = -PLAYER_FIRE_RESISTANCE;
	p->fall_distance = 0.0F;
}

/* Beta cadence for a lone 1-point hazard: hits at ticks 0, 10, 20, ... */
static int beta_suffocation_health(int tick) {
	return MAX_PLAYER_HEALTH - (tick / 10 + 1) * (HEALTH_PER_HEART / 2);
}

static void expect_cadence(struct server_local* s, struct server_player* p,
						   const char* what, int ticks) {
	for(int t = 0; t < ticks; t++) {
		settle_server(s, 1);
		s_checks++;
		if(p->health != beta_suffocation_health(t)) {
			char msg[96];
			snprintf(msg, sizeof(msg), "tick %d health %d want %d", t,
					 p->health, beta_suffocation_health(t));
			fail(what, msg);
		}
	}
}

static void expect_unhurt(struct server_local* s, struct server_player* p,
						  const char* what, int ticks) {
	settle_server(s, (unsigned)ticks);
	s_checks++;
	if(p->health != MAX_PLAYER_HEALTH) {
		char msg[96];
		snprintf(msg, sizeof(msg), "health %d, want untouched %d", p->health,
				 MAX_PLAYER_HEALTH);
		fail(what, msg);
	}
}

#define GROUND_Y 3
#define FEET (GROUND_Y + 1)

static void test_suffocation(void) {
	g_drain_run = 1;
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	struct server_local* s = split_fixture();
	s->rand_src = (struct random_gen) {.seed = 24680};
	server_world_test_set_gen_elapsed_mode(SERVER_WORLD_TEST_GEN_ELAPSED_IGNORE);
	server_local_test_set_near_send_minimum(4);
	server_local_test_expire_near_budget(true);
	s->player.active_inventory = &s->player.inventory;
	s->input_ack_pending = 1;
	s->player.finished_loading = true;
	settle_server(s, 400);
	s->input_ack_pending = 0;
	struct server_player* host = &s->player;

	/* Head cell (feet 4: samples at y 5.69 / 5.79 -> cell 5). */
	reset_body(host, 0.5, FEET, 0.5);
	put(s, 0, FEET + 1, 0, BLOCK_STONE);
	expect_cadence(s, host, "P1 host stone at head", 41);
	put(s, 0, FEET + 1, 0, BLOCK_AIR);

	/* Any isBlockNormalCube, not just stone. */
	reset_body(host, 0.5, FEET, 0.5);
	put(s, 0, FEET + 1, 0, BLOCK_COBBLESTONE);
	expect_cadence(s, host, "P1 host cobblestone at head", 12);
	put(s, 0, FEET + 1, 0, BLOCK_AIR);

	/* W1A-b: isBlockNormalCube is material + renderAsNormalBlock, not the
	 * render-opacity flag. The mob spawner (Material.rock, renderAsNormalBlock
	 * inherited true; Block.java:75, BlockMobSpawner.java:7) suffocates;
	 * glowstone is Material.rock in b1.7.3 (Block.java:112) and suffocates. */
	const uint8_t rock_cubes[] = {BLOCK_MOB_SPAWNER, BLOCK_GLOWSTONE};
	for(size_t k = 0; k < sizeof(rock_cubes); k++) {
		reset_body(host, 0.5, FEET, 0.5);
		put(s, 0, FEET + 1, 0, rock_cubes[k]);
		expect_cadence(s, host,
			k == 0 ? "P1 host mob spawner at head" : "P1 host glowstone at head",
			12);
		put(s, 0, FEET + 1, 0, BLOCK_AIR);
	}

	/* Negative controls: the FEET cell is never sampled; non-normal cubes. */
	reset_body(host, 0.5, FEET, 0.5);
	put(s, 0, FEET, 0, BLOCK_STONE);
	expect_unhurt(s, host, "P1 stone at feet only", 25);
	put(s, 0, FEET, 0, BLOCK_AIR);
	const uint8_t see_through[] = {BLOCK_GLASS, BLOCK_SLAB, BLOCK_TNT};
	for(size_t k = 0; k < sizeof(see_through); k++) {
		reset_body(host, 0.5, FEET, 0.5);
		put(s, 0, FEET + 1, 0, see_through[k]);
		expect_unhurt(s, host, "P1 non-normal-cube at head", 12);
		put(s, 0, FEET + 1, 0, BLOCK_AIR);
	}

	/* The +-0.05F eye band: feet +0.25 puts the upper row at y 6.04 (cell 6);
	 * feet +0.20 keeps it at 5.99 (cell 5). */
	put(s, 0, FEET + 2, 0, BLOCK_STONE);
	reset_body(host, 0.5, FEET + 0.20, 0.5);
	expect_unhurt(s, host, "P1 eye band below cell 6", 12);
	reset_body(host, 0.5, FEET + 0.25, 0.5);
	expect_cadence(s, host, "P1 eye band into cell 6", 12);
	put(s, 0, FEET + 2, 0, BLOCK_AIR);

	/* The +-0.27F (width*0.9F/2) horizontal band: x 0.75 reaches x=1.02. */
	put(s, 1, FEET + 1, 0, BLOCK_STONE);
	reset_body(host, 0.70, FEET, 0.5);
	expect_unhurt(s, host, "P1 x band short of the neighbour", 12);
	reset_body(host, 0.75, FEET, 0.5);
	expect_cadence(s, host, "P1 x band into the neighbour", 12);
	put(s, 1, FEET + 1, 0, BLOCK_AIR);
	reset_body(host, 0.5, FEET, 0.5);
	section_done("P1 host suffocation: cadence, sample geometry, controls");

	/* Split-screen guest: the same server-authoritative check. */
	static struct server_player guest;
	memset(&guest, 0, sizeof(guest));
	assert(inventory_create(&guest.inventory, &inventory_logic_player, s,
							&guest, INVENTORY_SIZE, 0, 0, 0));
	guest.active_inventory = &guest.inventory;
	guest.has_pos = true;
	guest.finished_loading = true;
	guest.dimension = host->dimension;
	reset_body(&guest, 4.5, FEET, 4.5);
	s->guest_actor = &guest;
	s->guest_gameplay = true;
	put(s, 4, FEET + 1, 4, BLOCK_STONE);
	expect_cadence(s, &guest, "P1 guest stone at head", 21);
	if(host->health != MAX_PLAYER_HEALTH)
		fail("P1 guest isolation", "guest suffocation leaked to the host");
	put(s, 4, FEET + 1, 4, BLOCK_AIR);
	s->guest_gameplay = false;
	s->guest_actor = NULL;
	inventory_destroy(&guest.inventory, &guest);
	section_done("P1 guest suffocation (server-authoritative, host untouched)");

	split_destroy(s);
	g_drain_run = 0;
	thread_join(&drain);
}

/* ======================= W1A-b (next-gaps-0924) ========================== */

/* Beta rotationYaw is -deg(orient[0]) (the SRPC_PLAYER_POS mapping). Find the
 * float orient whose Beta yaw is EXACTLY `yaw`, so the golden's input is the
 * Java float the table sees. */
static float orient_for_beta_yaw(float yaw) {
	float r = glm_rad(-yaw);
	for(int k = 0; k < 64; k++) {
		if(-glm_deg(r) == yaw)
			return r;
		r = nextafterf(r, -glm_deg(r) < yaw ? -INFINITY : INFINITY);
	}
	assert(!"no exact orient for this Beta yaw");
	return r;
}

/* Entity.moveFlying rotation through MathHelper.SIN_TABLE (MathHelper.java:6-12,
 * 56-61): open air, accel 0.02F, drag 0.91F. dx/dz are displacements from the
 * start. Beta moveStrafe is +1 for LEFT (MovementInputFromOptions:61-67). */
struct trig_case {
	float yaw;
	bool forward, right;
	uint32_t vx0_bits, vz0_bits; /* float bits of motion after tick 0 */
	double rows[4][4];           /* dx, vx, dz, vz */
};
static const struct trig_case k_trig[] = {
	{30.0F, true, false, 3155303099u, 1014830134u,
	 {{-0.009799457, -0.008917506, 0.016974410, 0.015446714},
	  {-0.028516421, -0.017032437, 0.049395534, 0.029503223},
	  {-0.055348315, -0.024417025, 0.095873167, 0.042294648},
	  {-0.089564797, -0.031136999, 0.155142225, 0.053934844}}},
	{30.0F, true, true, 3163554637u, 999972687u,
	 {{-0.019318350, -0.017579699, 0.005176998, 0.004711069},
	  {-0.056216398, -0.033577225, 0.015065065, 0.008998141},
	  {-0.109111973, -0.048134974, 0.029240204, 0.012899377},
	  {-0.176565297, -0.061382527, 0.047316579, 0.016449502}}},
	{-123.4F, true, true, 1016204386u, 997187168u,
	 {{0.019591395, 0.017828170, 0.004022093, 0.003660105},
	  {0.057010959, 0.034051805, 0.011704292, 0.006990801},
	  {0.110654159, 0.048815313, 0.022717187, 0.010021734},
	  {0.179060867, 0.062250106, 0.036761014, 0.012779884}}},
	{200.5F, false, true, 1015602479u, 1003266435u,
	 {{0.018359374, 0.016707031, 0.006862461, 0.006244840},
	  {0.053425780, 0.031910430, 0.019969762, 0.011927644},
	  {0.103695584, 0.045745523, 0.038759867, 0.017098996},
	  {0.167800481, 0.058335458, 0.062721325, 0.021804927}}},
};

static uint32_t float_bits(float f) {
	uint32_t u;
	memcpy(&u, &f, sizeof(u));
	return u;
}

static void test_movefly_sin_table(void) {
	for(size_t c = 0; c < sizeof(k_trig) / sizeof(k_trig[0]); c++) {
		const struct trig_case* k = &k_trig[c];
		struct entity e;
		walker(&e, 12.5F, 104.0F, 5.5F, false, 0.0F);
		e.orient[0] = orient_for_beta_yaw(k->yaw);
		const float x0 = e.pos[0], z0 = e.pos[2];
		for(int t = 0; t < 4; t++) {
			struct input_player_frame frame = {.connected = true};
			frame.down[IB_FORWARD] = k->forward;
			frame.down[IB_RIGHT] = k->right;
			assert(!entity_local_player_walk_owned(&e, &s_owner, &frame));
			if(t == 0) {
				s_checks++;
				if(float_bits(e.vel[0]) != k->vx0_bits
				   || float_bits(e.vel[2]) != k->vz0_bits) {
					char msg[160];
					snprintf(msg, sizeof(msg),
							 "yaw %.1f tick 0 motion %.9e/%.9e not bit-exact",
							 k->yaw, e.vel[0], e.vel[2]);
					fail("W1A-b MathHelper table", msg);
				}
			}
			near_or_die("W1A-b table dx", t, e.pos[0] - x0, k->rows[t][0],
						POS_TOL);
			near_or_die("W1A-b table vx", t, e.vel[0], k->rows[t][1],
						VEL_TOL);
			near_or_die("W1A-b table dz", t, e.pos[2] - z0, k->rows[t][2],
						POS_TOL);
			near_or_die("W1A-b table vz", t, e.vel[2], k->rows[t][3],
						VEL_TOL);
		}
	}
	section_done("W1A-b moveFlying via MathHelper.SIN_TABLE (bit-exact tick 0)");
}

/* No 0.005 cutoff: motionZ 0.004F in air keeps moving (x0.91F per tick). */
static void test_no_motion_cutoff(void) {
	static const struct zv want[] = {
		{0.004000000, 0.003640000}, {0.007640000, 0.003312400},
		{0.010952401, 0.003014284}, {0.013966685, 0.002742999},
	};
	struct entity e;
	walker(&e, 12.5F, 104.0F, 5.5F, false, 0.0F);
	e.vel[2] = 0.004F;
	const float z0 = e.pos[2];
	for(int t = 0; t < 4; t++) {
		step(&e, false, false);
		near_or_die("W1A-b no cutoff vz", t, e.vel[2], want[t].vz, VEL_TOL);
		near_or_die("W1A-b no cutoff dz", t, e.pos[2] - z0, want[t].z,
					POS_TOL);
	}
	section_done("W1A-b no |motion| < 0.005 cutoff");
}

/* No jump cooldown: EntityLiving.onLivingUpdate:625-634 calls jump()
 * (motionY = 0.42F, :659-661) on EVERY tick that is onGround while jump is
 * held. The harness lands the body back on its floor right after a jump (the
 * landing is not what is under test), and the very next held tick must jump
 * again: motionY after the tick = (0.42F - 0.08D) * (double)0.98F. */
static void test_jump_no_cooldown(void) {
	const double jumped_vy = ((double)0.42F - 0.08) * (double)0.98F;
	fill(2, 8, 100, 100, 2, 14, BLOCK_STONE, 0);
	struct entity e;
	walker(&e, 5.5F, 101.0F, 5.5F, true, -0.0784F);
	for(int t = 0; t < 4; t++) {
		struct input_player_frame frame = {.connected = true};
		frame.down[IB_JUMP] = true;
		assert(!entity_local_player_walk_owned(&e, &s_owner, &frame));
		near_or_die("W1A-b jump on every ground tick (motionY)", t, e.vel[1],
					jumped_vy, VEL_TOL);
		/* Land: back on the floor, grounded, resting motionY. */
		e.pos[1] = 101.0F + 1.62F;
		e.vel[1] = -0.0784F;
		e.on_ground = true;
	}
	fill(2, 8, 100, 100, 2, 14, BLOCK_AIR, 0);
	section_done("W1A-b jump has no cooldown (jumps again on the next ground tick)");
}

/* EntityPlayerSP.pushOutOfBlocks:198-250 (called 4x at onLivingUpdate:83-86).
 * posX 5.3F / posZ 5.5F, feet 101, body inside stone (5,101..102,5); stone
 * also at x=6 and z=4, open at x=4 and z=6. Samples (x-0.21, z+-0.21) pick -X
 * (frac 0.09); (x+0.21, z-0.21) picks -X (0.51 < 0.71); (x+0.21, z+0.21)
 * picks +Z (0.29 < 0.51). So motionX = -0.1F, motionZ = +0.1F, then the
 * ground friction 0.6F*0.91F (stone under): -0.054600008F / +0.054600008F,
 * and the free move lands at x 5.2, z 5.6. */
static void test_push_out_of_blocks(void) {
	fill(3, 7, 100, 100, 3, 7, BLOCK_STONE, 0);
	for(int y = 101; y <= 102; y++) {
		world_set_block(&gstate.world, 5, y, 5,
						(struct block_data) {.type = BLOCK_STONE}, false);
		world_set_block(&gstate.world, 6, y, 5,
						(struct block_data) {.type = BLOCK_STONE}, false);
		world_set_block(&gstate.world, 5, y, 4,
						(struct block_data) {.type = BLOCK_STONE}, false);
	}
	struct entity e;
	walker(&e, 5.3F, 101.0F, 5.5F, true, -0.0784F);
	step(&e, false, false);
	near_or_die("W1A-b pushOut vx", 0, e.vel[0], -0.054600008, VEL_TOL);
	near_or_die("W1A-b pushOut vz", 0, e.vel[2], 0.054600008, VEL_TOL);
	near_or_die("W1A-b pushOut x", 0, e.pos[0], 5.2, POS_TOL);
	near_or_die("W1A-b pushOut z", 0, e.pos[2], 5.6, POS_TOL);
	/* Negative control: a clear body is never pushed. */
	walker(&e, 4.3F, 101.0F, 6.5F, true, -0.0784F);
	step(&e, false, false);
	near_or_die("W1A-b pushOut control vx", 0, e.vel[0], 0.0, 0.0);
	near_or_die("W1A-b pushOut control vz", 0, e.vel[2], 0.0, 0.0);
	fill(3, 7, 100, 102, 3, 7, BLOCK_AIR, 0);
	section_done("W1A-b EntityPlayerSP.pushOutOfBlocks");
}

/* First join of a new world: Minecraft.changeWorld:1284-1287 ->
 * Entity.preparePlayerToSpawn:96-110 pushes a buried player up one block at a
 * time until getCollidingBoundingBoxes is empty. A log trunk y=4..7 plus
 * leaves y=8..9 on the spawn column, feet at 4: pushes 4..9 collide, feet 10
 * is clear (the leaf top at 10 only touches) -> feet 10.0 exactly, and no
 * suffocation tick follows. A SAVED world (LastPlayed != 0, written by the
 * same checkpoint that writes Player) loads its player where it was saved --
 * Beta pushes only a NEW player (World.spawnPlayerWithLoadedChunks:256-276). */
#define JOIN_X 8.5
#define JOIN_Z 8.5
static char s_join_dir[128];
static struct server_local s_join;

static void join_load(string_t lvl) {
	assert(server_local_load_world_impl(&s_join, lvl));
	server_local_test_finish_load_inventory(&s_join);
}

static void test_first_join_push_up(void) {
	g_drain_run = 1;
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	test_fixture_dir("test_player_first_join", s_join_dir, sizeof(s_join_dir));
#ifdef _WIN32
	_mkdir(s_join_dir);
#else
	mkdir(s_join_dir, 0777);
#endif
	server_local_test_create_without_thread(&s_join, false, false, false,
		false, false, true /*superflat*/, false);
	string_t lvl;
	string_init_set_str(lvl, s_join_dir);
	{
		struct level_archive la;
		assert(level_archive_create_new(&la, lvl, 4242,
			(vec3) {JOIN_X, 4.0F + PLAYER_EYE_HEIGHT, JOIN_Z}));
		level_archive_destroy(&la);
	}
	/* Fixture: build the tree on the spawn column and bury the player in it,
	 * then put the level back into its never-saved state (LastPlayed 0, as
	 * the creation template has it) -- i.e. a brand-new world whose
	 * decoration landed a trunk on the creation-time spawn. */
	join_load(lvl);
	{
		struct server_chunk* sc = NULL;
		if(!server_world_is_chunk_loaded(&s_join.world, 0, 0))
			assert(server_world_load_chunk(&s_join.world, 0, 0, &sc) && sc);
	}
	for(int y = 4; y <= 9; y++) {
		struct block_data want = {.type = y <= 7 ? BLOCK_LOG : BLOCK_LEAVES};
		(void)server_world_set_block(&s_join.worlds[0], 8, y, 8, want);
		struct block_data live;
		assert(server_world_get_block(&s_join.world, 8, y, 8, &live)
			   && live.type == want.type);
	}
	s_join.player.x = JOIN_X;
	s_join.player.y = 4.0 + (double)PLAYER_EYE_HEIGHT;
	s_join.player.z = JOIN_Z;
	server_local_unload_world_impl(&s_join, true);
	{
		struct level_archive la;
		assert(level_archive_create(&la, lvl));
		int64_t never = 0;
		assert(level_archive_write(&la, LEVEL_LAST_PLAYED, &never));
		level_archive_destroy(&la);
	}

	join_load(lvl);
	near_or_die("W1A-b first join feet", 0,
				s_join.player.y - (double)PLAYER_EYE_HEIGHT, 10.0, 1e-5);
	s_join.player.finished_loading = true;
	s_join.input_ack_pending = 0;
	s_join.paused = false;
	for(int t = 0; t < 12; t++)
		server_local_test_update_once(&s_join);
	s_checks++;
	if(s_join.player.health != MAX_PLAYER_HEALTH)
		fail("W1A-b first join", "suffocated after the push-up");

	/* Control: a SAVED world keeps its saved (buried) player. The unload's
	 * own checkpoint writes LastPlayed, exactly as in play. */
	s_join.player.y = 4.0 + (double)PLAYER_EYE_HEIGHT;
	s_join.player.health = MAX_PLAYER_HEALTH;
	server_local_unload_world_impl(&s_join, true);
	join_load(lvl);
	near_or_die("W1A-b played world keeps saved feet", 0,
				s_join.player.y - (double)PLAYER_EYE_HEIGHT, 4.0, 1e-5);
	server_local_unload_world_impl(&s_join, true);
	string_clear(lvl);
	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(s_join_dir);
	section_done("W1A-b first-join push-up (tree trunk), played-world control");
}

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();

	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);

	test_ground_friction();
	test_fluid_and_air_accel();
	test_ladder();
	test_embedded_recovery();
	test_suffocation();
	test_movefly_sin_table();
	test_no_motion_cutoff();
	test_jump_no_cooldown();
	test_push_out_of_blocks();
	test_first_join_push_up();

	if(s_failures) {
		fprintf(stderr, "test_player_move_parity_golden: %d of %d checks FAILED\n",
				s_failures, s_checks);
		return 1;
	}
	printf("PASS test_player_move_parity_golden (%d checks)\n", s_checks);
	return 0;
}
