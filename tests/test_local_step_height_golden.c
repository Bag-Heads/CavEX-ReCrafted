/*
	test_local_step_height_golden -- next-gaps-0924/STEPCAM. FAITHFULNESS golden
	for the local player's step HEIGHT, through the real walk.

	Spec (Beta 1.7.3, ../mcb173-retromcp/minecraft/src_original/net/minecraft/src):
	  EntityLiving.<init>:67       stepHeight = 0.5F;
	  Entity.moveEntity:346        step route rises dy = (double)stepHeight
	  Entity.moveEntity:390        then comes back down dy = -stepHeight
	  BlockBed.java:132            bed bounds 0..9/16 high (0.5625)
	  AxisAlignedBB.calculateZOffset: only boxes that overlap in Y (strictly)
	      block a Z move, so a box raised by exactly 0.5 clears a 0.5 top and
	      does NOT clear a 0.5625 top.

	So: walking into a slab (rise 0.5) steps up; walking into a bed (rise
	0.5625, the smallest rise above 0.5 that Beta block geometry can make --
	every collision top is on the 1/16 grid, so a 0.55 lip cannot be built)
	stops at the bed's face with the eye unchanged; a full block stops too.

	This port stores the body as a float EYE. A plain eye += 0.5F can round the
	feet one ulp SHORT of feet + 0.5 (24 of 2016 grid floors at y 1..126, e.g.
	feet 62.0 -> 62.4999962), which would make that slab un-steppable -- the
	y=62 slab case below pins that the port reaches the full 0.5.

	RED on the 0.6 walk (measured): the player climbs onto the bed.
	Every expected coordinate is bit-exact to the Beta rule in this port's
	float placement (see test_local_step_gate_golden.c for the helpers' basis).
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "entity/entity.h"
#include "entity/player_ysize.h"
#include "game/client_session.h"
#include "game/game_state.h"
#include "item/items.h"
#include "network/server_local_state.h" /* MAX_OXYGEN */
#include "platform/input_owner.h"
#include "world.h"

static int s_checks, s_failures;

static uint32_t bits(float value) {
	uint32_t result;
	memcpy(&result, &value, sizeof(result));
	return result;
}

static void feq(const char* what, float got, float want) {
	s_checks++;
	if(bits(got) != bits(want)) {
		s_failures++;
		fprintf(stderr, "FAIL %s: got %.9g (0x%08X) want %.9g (0x%08X)\n",
				what, (double)got, (unsigned)bits(got), (double)want,
				(unsigned)bits(want));
	}
}

static struct client_session s_owner;

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

static float box_lo_y(void) {
	volatile float lo = -(1.8F / 2.0F);
	lo += 1.8F / 2.0F - 1.62F;
	return lo;
}

/* entity_try_move's clipped-axis placement (entity.c). */
static float place(float face, float lo, float hi, bool positive) {
	float np = face - (positive ? hi : lo);
	if(positive ? np + hi > face : np + lo < face)
		np = nextafterf(np, positive ? -INFINITY : INFINITY);
	return np;
}

static float eye_on(float face) {
	return place(face, box_lo_y(), 0.0F, false);
}

static float z_against(float face) {
	return place(face, -0.3F, 0.3F, true);
}

/* Floor top at `feet`, the obstacle row at z = 8, player box front at 7.95,
 * one tick of +z 0.2 requested (no input; motionY the standing -0.0784F). */
static void run(const char* name, int feet, uint8_t type, float obstacle_h,
				bool steps) {
	cfill(2, 10, feet, feet + 3, 2, 14, BLOCK_AIR, 0);
	cfill(2, 10, feet - 1, feet - 1, 2, 14, BLOCK_STONE, 0);
	cfill(2, 10, feet, feet, 8, 8, type, 0);
	const int fail0 = s_failures;

	struct entity e;
	memset(&e, 0, sizeof e);
	entity_local_player(91, &e, &gstate.world);
	e.pos[0] = 6.5F;
	e.pos[1] = eye_on((float)feet);
	e.pos[2] = 7.65F;
	e.orient[1] = GLM_PI_2;
	e.on_ground = true;
	e.data.local_player.capture_input = true;
	memset(&s_owner, 0, sizeof s_owner);
	s_owner.local_player = &e;
	s_owner.oxygen = MAX_OXYGEN;

	const float eye = e.pos[1];
	const float z = e.pos[2];
	e.vel[0] = 0.0F;
	e.vel[1] = -0.0784F;
	e.vel[2] = 0.2F;
	struct input_player_frame frame = {.connected = true};
	assert(!entity_local_player_walk_owned(&e, &s_owner, &frame));

	if(steps) {
		feq("steps: eye on the obstacle top", e.pos[1],
			eye_on((float)feet + obstacle_h));
		feq("steps: z moved the full 0.2", e.pos[2], z + 0.2F);
	} else {
		feq("blocked: eye unchanged", e.pos[1], eye);
		feq("blocked: z stops at the obstacle face", e.pos[2],
			z_against(8.0F));
		feq("blocked: no ySize bank", s_owner.camera.ysize.y_size, 0.0F);
	}
	printf("%s %s\n", s_failures == fail0 ? "PASS" : "RED ", name);
}

/* ------------------------------------------------------------------------
 * Step-route origin and entry cases (Astra review of fefeda0b, section B).
 *
 * Beta runs the whole step route from box0, the box as it was BEFORE the
 * flat pass (Entity.moveEntity:274, :350 setBB(box0)), offsetting it by
 * exactly +stepHeight, the requested x/z, and -stepHeight (:346, :390) --
 * box-minY arithmetic in double. After an accepted step (:412-429):
 *     isCollidedVertically = ry != dy;
 *     onGround = ry != dy && ry < 0.0D;
 *     motionY = 0.0D when ry != dy;
 * where ry is the REQUESTED dy and dy the step route's final (down)
 * component. The walk then applies motionY -= 0.08F; motionY *= 0.98F.
 * ---------------------------------------------------------------------- */

/* One tick from an explicit eye and motion; on_ground as given. */
static void tick_from(struct entity* e, float eye, bool on_ground, float vy,
					  float vz) {
	memset(e, 0, sizeof *e);
	entity_local_player(91, e, &gstate.world);
	e->pos[0] = 6.5F;
	e->pos[1] = eye;
	e->pos[2] = 7.65F; /* box front 7.95 */
	e->orient[1] = GLM_PI_2;
	e->on_ground = on_ground;
	e->data.local_player.capture_input = true;
	memset(&s_owner, 0, sizeof s_owner);
	s_owner.local_player = e;
	s_owner.oxygen = MAX_OXYGEN;
	e->vel[0] = 0.0F;
	e->vel[1] = vy;
	e->vel[2] = vz;
	struct input_player_frame frame = {.connected = true};
	assert(!entity_local_player_walk_owned(e, &s_owner, &frame));
}

static void beq(const char* what, bool got, bool want) {
	s_checks++;
	if(got != want) {
		s_failures++;
		fprintf(stderr, "FAIL %s: got %d want %d\n", what, got, want);
	}
}

/* The float motionY the walk leaves after Beta zeroed it (:424) and applied
 * gravity and drag: (0 - 0.08F) * 0.98F. */
static float vy_after_vertical_collision(void) {
	volatile float v = 0.0F;
	v -= 0.08F;
	v *= 0.98F;
	return v;
}

static void level(int feet, bool ceiling) {
	cfill(2, 10, feet - 1, feet + 4, 2, 14, BLOCK_AIR, 0);
	cfill(2, 10, feet - 1, feet - 1, 2, 14, BLOCK_STONE, 0);
	if(ceiling)
		cfill(2, 10, feet + 2, feet + 2, 2, 14, BLOCK_STONE, 0);
}

/* 1. JUMPING ORIGIN. Grounded at feet 62 (a float-short floor), jump
 * motionY 0.42, walking into a slab. Flat: rises 0.42, z blocked at 8.0.
 * Step route from the PRE-move box: 62.0 + 0.5 = 62.5 clears the slab top,
 * z free, the down pass is clipped at once (var3 = 0 != var13 = 0.42):
 * accepted, feet 62.5, onGround FALSE (var13 > 0), motionY zeroed.
 * RED on fefeda0b: the rise was computed from the flat-moved eye, the feet
 * ended at 62.4999962 and the slab still blocked the step. */
static void jump_origin(void) {
	level(62, false);
	cfill(2, 10, 62, 62, 8, 8, BLOCK_SLAB, 0);
	const int fail0 = s_failures;
	struct entity e;
	const float z0 = 7.65F;
	tick_from(&e, eye_on(62.0F), true, 0.42F, 0.2F);
	feq("jump: eye on the slab top", e.pos[1], eye_on(62.5F));
	feq("jump: z moved the full 0.2", e.pos[2], z0 + 0.2F);
	beq("jump: onGround = var13 != var3 && var13 < 0 -> false", e.on_ground,
		false);
	feq("jump: motionY zeroed (var13 != var3) then gravity", e.vel[1],
		vy_after_vertical_collision());
	feq("jump: ySize banked 0.5 + 0.01", s_owner.camera.ysize.y_size,
		(float)(0.0 + 0.5 + 0.01));
	printf("%s jump from the ground into a slab: step from the pre-move box\n",
		   s_failures == fail0 ? "PASS" : "RED ");
}

/* 2. AIRBORNE, DOWNWARD-CLIPPED ENTRY. Falling (onGround false) 0.1 above
 * the floor at 101 with motionY -0.2: the flat Y pass lands (the second
 * term of var36), z blocked by the slab. Step route from the airborne box:
 * up 0.5 clears 101.5, z free, down lands on 101.5. onGround true. */
static void airborne_entry(void) {
	level(101, false);
	cfill(2, 10, 101, 101, 8, 8, BLOCK_SLAB, 0);
	const int fail0 = s_failures;
	struct entity e;
	const float z0 = 7.65F;
	tick_from(&e, eye_on(101.0F) + 0.1F, false, -0.2F, 0.2F);
	feq("airborne: eye on the slab top", e.pos[1], eye_on(101.5F));
	feq("airborne: z moved the full 0.2", e.pos[2], z0 + 0.2F);
	beq("airborne: onGround true", e.on_ground, true);
	feq("airborne: motionY zeroed then gravity", e.vel[1],
		vy_after_vertical_collision());
	feq("airborne: ySize banked", s_owner.camera.ysize.y_size,
		(float)(0.0 + 0.5 + 0.01));
	printf("%s airborne landing entry: step route from the airborne box\n",
		   s_failures == fail0 ? "PASS" : "RED ");
}

/* 3. CEILING-LIMITED STEP THAT STILL SUCCEEDS. Standing on soul sand (top
 * 100.875, the 7/8 BlockSoulSand box), stone ceiling at y=103: the up pass
 * is clipped to 103 - 1.8 - 100.875 = 0.325; a full block at z=8 (top 101.0,
 * rise 0.125) is still cleared; the down pass (-0.5, not -0.325) lands on
 * 101.0. Whole minY -> no ySize bank. */
static void ceiling_limited_success(void) {
	cfill(2, 10, 99, 104, 2, 14, BLOCK_AIR, 0);
	cfill(2, 10, 99, 99, 2, 14, BLOCK_STONE, 0);
	cfill(2, 10, 100, 100, 2, 7, BLOCK_SOULSAND, 0);
	cfill(2, 10, 100, 100, 8, 14, BLOCK_STONE, 0);
	cfill(2, 10, 103, 103, 2, 14, BLOCK_STONE, 0);
	const int fail0 = s_failures;
	struct entity e;
	const float z0 = 7.65F;
	tick_from(&e, eye_on(100.875F), true, -0.0784F, 0.2F);
	feq("ceiling-limited: eye on the block top", e.pos[1], eye_on(101.0F));
	feq("ceiling-limited: z moved the full 0.2", e.pos[2], z0 + 0.2F);
	beq("ceiling-limited: onGround true", e.on_ground, true);
	feq("ceiling-limited: no bank at a whole minY",
		s_owner.camera.ysize.y_size, 0.0F);
	printf("%s ceiling-limited rise (0.325) still steps a 0.125 lip\n",
		   s_failures == fail0 ? "PASS" : "RED ");
}

/* 4. CEILING-BLOCKED STEP = HORIZONTAL TIE. Feet 101, ceiling at y=103:
 * the up pass stops at 0.2, the raised box (101.2) is still stopped by the
 * slab (101.5) at the same face as the flat pass. Equal horizontal travel
 * -> the >= at :398 keeps the FLAT pass. */
static void ceiling_blocked_tie(void) {
	level(101, true);
	cfill(2, 10, 101, 101, 8, 8, BLOCK_SLAB, 0);
	const int fail0 = s_failures;
	struct entity e;
	const float eye = eye_on(101.0F);
	tick_from(&e, eye, true, -0.0784F, 0.2F);
	feq("ceiling tie: eye unchanged (flat kept)", e.pos[1], eye);
	feq("ceiling tie: z stops at the slab face", e.pos[2], z_against(8.0F));
	feq("ceiling tie: no bank", s_owner.camera.ysize.y_size, 0.0F);
	printf("%s ceiling-blocked step ties -> flat pass kept\n",
		   s_failures == fail0 ? "PASS" : "RED ");
}

/* An eye whose derived feet fl(eye + lo) is exactly `feet` (at y ~100 the
 * eye and the feet share a binade, so every feet value is reachable). */
static float eye_with_feet(float feet) {
	const float lo = box_lo_y();
	float eye = feet - lo;
	for(int i = 0; i < 8 && eye + lo != feet; i++)
		eye = nextafterf(eye, eye + lo < feet ? INFINITY : -INFINITY);
	assert(eye + lo == feet);
	return eye;
}

/* 5. ROUNDING BOUNDARY / TIE at the bed. Airborne feet f just around
 * 100.0625, motionY -0.1 (the flat pass lands on 100.0, z blocked by the
 * bed). Beta: f + 0.5 against the bed top 100.5625 with STRICT overlap --
 *   f = 100.0625 - 1 ulp: 100.5624996 < 100.5625 -> blocked, horizontal
 *       tie -> flat kept (feet 100.0, z at the bed face);
 *   f = 100.0625 exactly: box minY == bed top -> no overlap -> steps;
 *   f = 100.0625 + 1 ulp: steps.
 * The step lands on the bed top (100.5625). */
static void bed_boundary(void) {
	level(100, false);
	cfill(2, 10, 100, 100, 8, 8, BLOCK_BED, 0);
	const float at = 100.0625F;
	const float below = nextafterf(at, -INFINITY);
	const float above = nextafterf(at, INFINITY);
	const float starts[3] = {below, at, above};
	const bool steps[3] = {false, true, true};
	const char* names[3] = {"-1 ulp", "exact", "+1 ulp"};
	for(int i = 0; i < 3; i++) {
		const int fail0 = s_failures;
		struct entity e;
		const float z0 = 7.65F;
		tick_from(&e, eye_with_feet(starts[i]), false, -0.1F, 0.2F);
		if(steps[i]) {
			feq("bed boundary: eye on the bed top", e.pos[1],
				eye_on(100.5625F));
			feq("bed boundary: z moved the full 0.2", e.pos[2], z0 + 0.2F);
		} else {
			feq("bed boundary: flat kept, landed on the floor", e.pos[1],
				eye_on(100.0F));
			feq("bed boundary: z at the bed face", e.pos[2], z_against(8.0F));
		}
		printf("%s bed boundary, airborne feet 100.0625 %s: %s\n",
			   s_failures == fail0 ? "PASS" : "RED ", names[i],
			   steps[i] ? "steps" : "blocked (tie keeps flat)");
	}
}

/* 6. REAL-WALK SWEEP over every integer floor the player can stand on
 * (feet 2..124, including the binade-split rows 14/30/62/126-adjacent):
 * walking into a slab always steps onto exactly its top, walking into a bed
 * always stops at its face. Quiet unless a floor fails. */
static void floor_sweep(void) {
	const int fail0 = s_failures;
	int floors = 0;
	for(int feet = 2; feet <= 124; feet++) {
		const uint8_t type[2] = {BLOCK_SLAB, BLOCK_BED};
		for(int t = 0; t < 2; t++) {
			level(feet, false);
			cfill(2, 10, feet, feet, 8, 8, type[t], 0);
			struct entity e;
			const float eye = eye_on((float)feet);
			tick_from(&e, eye, true, -0.0784F, 0.2F);
			if(t == 0) {
				feq("sweep: slab steps onto its top", e.pos[1],
					eye_on((float)feet + 0.5F));
				feq("sweep: slab z free", e.pos[2], 7.65F + 0.2F);
			} else {
				feq("sweep: bed stops, eye unchanged", e.pos[1], eye);
				feq("sweep: bed z at the face", e.pos[2], z_against(8.0F));
			}
			cfill(2, 10, feet - 1, feet + 4, 2, 14, BLOCK_AIR, 0);
		}
		floors++;
	}
	printf("%s real-walk floor sweep: %d floors x {slab steps, bed stops}\n",
		   s_failures == fail0 ? "PASS" : "RED ", floors);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);

	run("slab (rise 0.5) at feet 101: steps", 101, BLOCK_SLAB, 0.5F, true);
	run("slab (rise 0.5) at feet 62 (float-short floor): steps", 62,
		BLOCK_SLAB, 0.5F, true);
	run("slab (rise 0.5) at feet 14 (float-short floor): steps", 14,
		BLOCK_SLAB, 0.5F, true);
	run("bed (rise 0.5625 > stepHeight 0.5F) at feet 101: stops", 101,
		BLOCK_BED, 0.5625F, false);
	run("bed (rise 0.5625) at feet 62: stops", 62, BLOCK_BED, 0.5625F, false);
	run("full block (rise 1.0): stops", 101, BLOCK_STONE, 1.0F, false);
	jump_origin();
	airborne_entry();
	ceiling_limited_success();
	ceiling_blocked_tie();
	bed_boundary();
	floor_sweep();

	if(s_failures) {
		fprintf(stderr, "test_local_step_height_golden: %d of %d checks FAILED\n",
				s_failures, s_checks);
		return 1;
	}
	printf("PASS test_local_step_height_golden (%d checks)\n", s_checks);
	return 0;
}
