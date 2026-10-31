/*
	test_local_step_gate_golden -- next-gaps-0924/STEPCAM follow-up. FAITHFULNESS
	golden for WHEN the local player's step route runs and WHEN it is kept,
	driven through the real walk (entity_local_player_walk_owned).

	Spec (Beta 1.7.3, ../mcb173-retromcp/minecraft/src_original/net/minecraft/src,
	Entity.moveEntity):
	  :259  ySize *= 0.4F;                                  (first statement)
	  :271-274  rx/ry/rz = the requested dx/dy/dz; box0 = box copy
	  :276  sneakEdge = onGround && isSneaking();
	  :298-334  flat pass: Y, then X, then Z against one collision list
	  :311  grounded = onGround || ry != dy && ry < 0.0D;
	  :341  gate: stepHeight > 0.0F && grounded && (sneakEdge || ySize < 0.05F)
	            && (rx != dx || rz != dz)              -- the gate
	  :342-396  step route from box0: up stepHeight, X, Z, down -stepHeight
	  :398-402  when flatX^2 + flatZ^2 >= stepX^2 + stepZ^2
	                { restore the flat result }               -- HORIZONTAL only,
	            ties keep the flat pass
	  :403-409  else bank the fractional minY into ySize.
	EntityPlayerSP.isSneaking:144-146 = movementInput.sneak && !sleeping.

	Each tick below sets the requested motion directly (no input), so the move
	is exactly the vector written and every expected coordinate is the Beta
	rule applied in this port's float representation: an unclipped axis moves
	pos + d, a clipped axis lands on the face with entity_try_move's placement
	(face - extent, nudged one ulp to the safe side), and the chosen route is
	the one the decompile's gate and horizontal comparison pick.

	RED on the pre-gate walk (measured): the double stair step climbs on the
	second tick instead of waiting for ySize < 0.05F, and the diagonal case
	takes the higher but horizontally SHORTER step route because the old
	acceptance compared 3D distance.
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

static void feq(const char* what, int tick, float got, float want) {
	s_checks++;
	if(bits(got) != bits(want)) {
		s_failures++;
		fprintf(stderr, "FAIL %s tick %d: got %.9g (0x%08X) want %.9g (0x%08X)\n",
				what, tick, (double)got, (unsigned)bits(got), (double)want,
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

/* The walk's body box relative to the eye (entity_local_player.c). */
static float box_lo_y(void) {
	volatile float lo = -(1.8F / 2.0F);
	lo += 1.8F / 2.0F - 1.62F;
	return lo;
}

/* entity_try_move's placement of a clipped axis onto `face` (entity.c):
 * np = face - (d > 0 ? hi : lo), nudged one ulp back to the safe side. */
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

static void spawn(struct entity* e, float x, float feet, float z) {
	memset(e, 0, sizeof *e);
	entity_local_player(91, e, &gstate.world);
	e->pos[0] = x;
	e->pos[1] = eye_on(feet);
	e->pos[2] = z;
	e->orient[1] = GLM_PI_2;
	e->on_ground = true;
	e->data.local_player.capture_input = true;
	memset(&s_owner, 0, sizeof s_owner);
	s_owner.local_player = e;
	s_owner.oxygen = MAX_OXYGEN;
}

/* One tick with the requested motion written straight in: no input, so the
 * walk's moveFlying adds nothing and the move is exactly (vx, vy, vz). */
static void push(struct entity* e, float vx, float vy, float vz, bool sneak) {
	e->vel[0] = vx;
	e->vel[1] = vy;
	e->vel[2] = vz;
	struct input_player_frame frame = {.connected = true};
	frame.down[IB_SNEAK] = sneak;
	assert(!entity_local_player_walk_owned(e, &s_owner, &frame));
}

/* Stone stairs, meta 2: lower half y 0..0.5 over the whole cell, upper half
 * y 0.5..1 at z 0.5..1 (block_stairs.c SIDE_BACK). Walking +z the rises are
 * HALF a block apart, so a player can meet the second one two ticks after the
 * first -- while the first step's ySize (0.51 -> 0.204 -> 0.0816) still
 * holds Beta's gate shut. */
static void double_stair_step(bool sneak) {
	const char* name = sneak ? "double stair step, sneak held"
							 : "double stair step, walking";
	cfill(2, 10, 101, 104, 2, 14, BLOCK_AIR, 0);
	cfill(2, 10, 100, 100, 2, 14, BLOCK_STONE, 0);
	cfill(2, 10, 101, 101, 8, 8, BLOCK_STONE_STAIRS, 2);
	const int fail0 = s_failures;

	struct entity e;
	spawn(&e, 6.5F, 101.0F, 7.65F); /* box front 7.95 */
	const float vy = -0.0784F;       /* Beta's standing motionY */
	/* Settle the sneak floor the way a held sneak would have. */
	if(sneak) {
		push(&e, 0.0F, vy, 0.0F, true);
		push(&e, 0.0F, vy, 0.0F, true);
	}
	const float lower = eye_on(101.5F);
	const float upper = eye_on(102.0F);

	/* Tick A: front 7.95 -> 8.15 hits the lower half; ySize < 0.05F (or
	 * sneaking) -> step onto 101.5. Horizontal 0.2 > flat 0.05. */
	float z = e.pos[2];
	push(&e, 0.0F, vy, 0.2F, sneak);
	feq("A eye on the lower half", 0, e.pos[1], lower);
	feq("A z moved the full 0.2", 0, e.pos[2], z + 0.2F);
	z = e.pos[2];

	/* Tick B: front 8.15 -> 8.35, free (upper half starts at 8.5). */
	push(&e, 0.0F, vy, 0.2F, sneak);
	feq("B eye stays on the lower half", 1, e.pos[1], lower);
	feq("B z free", 1, e.pos[2], z + 0.2F);
	z = e.pos[2];

	/* Tick C: front 8.35 -> 8.55 hits the upper half. ySize is 0.51F*0.4F*0.4F
	 * = 0.0816 >= 0.05F. Walking: the gate is SHUT (:341) -> flat pass only,
	 * z clipped at 8.5, eye unchanged. Sneaking: var18 opens it -> step. */
	push(&e, 0.0F, vy, 0.2F, sneak);
	if(sneak) {
		feq("C sneak: var18 opens the gate, eye on the upper half", 2,
			e.pos[1], upper);
		feq("C sneak: z free", 2, e.pos[2], z + 0.2F);
	} else {
		feq("C walk: gate shut (ySize 0.0816 >= 0.05F), eye unchanged", 2,
			e.pos[1], lower);
		feq("C walk: z clipped at the upper half's face", 2, e.pos[2],
			z_against(8.5F));
		z = e.pos[2];

		/* Tick D: ySize 0.0326 < 0.05F -> the gate opens, the step wins. */
		push(&e, 0.0F, vy, 0.2F, false);
		feq("D walk: ySize 0.0326 < 0.05F, eye on the upper half", 3,
			e.pos[1], upper);
		feq("D walk: z free", 3, e.pos[2], z + 0.2F);
	}
	printf("%s %s\n", s_failures == fail0 ? "PASS" : "RED ", name);
}

/* A diagonal move where the step route ends HIGHER but horizontally SHORTER
 * than the flat pass. Flat: x free (+0.2; the y=103 block is above the
 * standing box's top 102.8), z clipped by the slab face at 8.0 (dz 0.01).
 * Step: the raised box (top 103.4) is stopped in x by the y=103 block at
 * x=6 (dx 0.05), z is free over the slab (dz 0.05), and it lands on 101.5.
 * Horizontal: step 0.005 < flat 0.0401 -> Beta keeps the FLAT pass (:398).
 * The old 3D comparison (0.005 + 0.25 > 0.0401) took the step. */
static void diagonal_shorter_step(void) {
	cfill(2, 10, 101, 104, 2, 14, BLOCK_AIR, 0);
	cfill(2, 10, 100, 100, 2, 14, BLOCK_STONE, 0);
	cfill(2, 10, 101, 101, 8, 8, BLOCK_SLAB, 0);
	cfill(6, 6, 103, 103, 7, 7, BLOCK_STONE, 0);
	const int fail0 = s_failures;

	struct entity e;
	spawn(&e, 5.65F, 101.0F, 7.69F); /* box x 5.35..5.95, z 7.39..7.99 */
	const float eye = e.pos[1];
	const float x = e.pos[0];
	push(&e, 0.2F, -0.0784F, 0.05F, false);
	feq("diagonal: flat kept, eye unchanged", 0, e.pos[1], eye);
	feq("diagonal: flat x free", 0, e.pos[0], x + 0.2F);
	feq("diagonal: flat z clipped at the slab face", 0, e.pos[2],
		z_against(8.0F));
	feq("diagonal: no step, no ySize bank", 0, s_owner.camera.ysize.y_size,
		0.0F);
	printf("%s diagonal: horizontal (not 3D) distance keeps the flat pass\n",
		   s_failures == fail0 ? "PASS" : "RED ");

	/* Control, same geometry without the y=103 block: the step is
	 * horizontally longer (0.2^2 + 0.05^2 > 0.2^2 + 0.01^2) and wins. */
	cfill(6, 6, 103, 103, 7, 7, BLOCK_AIR, 0);
	const int fail1 = s_failures;
	spawn(&e, 5.65F, 101.0F, 7.69F);
	const float x1 = e.pos[0];
	const float z1 = e.pos[2];
	push(&e, 0.2F, -0.0784F, 0.05F, false);
	feq("diagonal control: step kept, eye on the slab", 1, e.pos[1],
		eye_on(101.5F));
	feq("diagonal control: x free", 1, e.pos[0], x1 + 0.2F);
	feq("diagonal control: z free", 1, e.pos[2], z1 + 0.05F);
	printf("%s diagonal control: longer horizontal step is kept\n",
		   s_failures == fail1 ? "PASS" : "RED ");
}

/* Gate term 4 (:341, var11 != var1 || var15 != var5): with NO horizontal
 * clip Beta never runs the step route, even walking off a slab edge where
 * the old walk's "up 0.6, across, down 0.6" landed higher than the falling
 * flat pass. Player on a slab edge (feet 101.5) moving +z off it with a
 * falling motionY: flat = free fall, not a step back onto the slab height. */
static void no_horizontal_clip_no_step(void) {
	cfill(2, 10, 101, 104, 2, 14, BLOCK_AIR, 0);
	cfill(2, 10, 100, 100, 2, 14, BLOCK_STONE, 0);
	cfill(2, 10, 101, 101, 6, 6, BLOCK_SLAB, 0);
	const int fail0 = s_failures;
	struct entity e;
	/* box z 6.75..7.35 -- still overlapping the slab cell (z < 7) */
	spawn(&e, 6.5F, 101.5F, 7.05F);
	const float eye = e.pos[1];
	const float z = e.pos[2];
	/* +z 0.1 takes the box to z 6.85..7.45: still over the slab, the flat
	 * Y pass clips on the slab top (no fall) and nothing is horizontally
	 * clipped, so the gate stays shut and the result is the flat pass. */
	push(&e, 0.0F, -0.0784F, 0.1F, false);
	feq("no clip: z free", 0, e.pos[2], z + 0.1F);
	feq("no clip: eye unchanged", 0, e.pos[1], eye);
	feq("no clip: no ySize bank", 0, s_owner.camera.ysize.y_size, 0.0F);
	printf("%s no horizontal clip -> no step route\n",
		   s_failures == fail0 ? "PASS" : "RED ");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	blocks_init();
	items_init();
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);

	double_stair_step(false);
	double_stair_step(true);
	diagonal_shorter_step();
	no_horizontal_clip_no_step();

	if(s_failures) {
		fprintf(stderr, "test_local_step_gate_golden: %d of %d checks FAILED\n",
				s_failures, s_checks);
		return 1;
	}
	printf("PASS test_local_step_gate_golden (%d checks)\n", s_checks);
	return 0;
}
