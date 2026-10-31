/*
 * Copyright (c) 2026 CaveWii_v2 contributors
 *
 * RED: Beta 1.7.3 Entity.moveEntity queries boundingBox.addCoord and clips
 * every obstacle with AxisAlignedBB.calculateX/Y/ZOffset (client Entity.java
 * moveEntity:300-328; AxisAlignedBB.java:104-195). The body's local bounds
 * are [-0.3,0,-0.3]..[0.3,1.8,0.3], with posY at its feet.
 */

#include <assert.h>
#include <math.h>
#include <stdbool.h>
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
#include "block/blocks_data.h"
#include "entity/entity.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_fixture_dir[128];
static struct server_local s;
static volatile int s_drain_run = 1;
static int s_failures;

static void* drain_entry(void* arg) {
	(void)arg;
	while(s_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void put(int x, int y, int z, uint8_t type, uint8_t meta) {
	struct block_data live;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &live));
	if(live.type != type || live.metadata != meta)
		assert(server_world_set_block_silent(&s.worlds[0], x, y, z,
			(struct block_data) {.type = type, .metadata = meta}));
	assert(server_world_get_block(&s.worlds[0], x, y, z, &live));
	assert(live.type == type && live.metadata == meta);
}

static void clear_corridor(void) {
	/* Clear the entire swept boxes, plus a cell of padding on every side.
	 * Generated superflat terrain must not decide these cases. */
	for(int x = 6; x <= 13; x++)
		for(int y = 54; y <= 69; y++)
			for(int z = 6; z <= 10; z++)
				put(x, y, z, BLOCK_AIR, 0);
}

static struct AABB body_box(void) {
	struct AABB box;
	aabb_setsize_centered_offset(&box, 0.6F, 1.8F, 0.6F,
		0.0F, 0.9F, 0.0F);
	return box;
}

static void reset_body(struct entity* e, float x, float feet, float z) {
	memset(e, 0, sizeof(*e));
	entity_default_init(e, true, &s.worlds[0]);
	e->pos[0] = x;
	e->pos[1] = feet;
	e->pos[2] = z;
}

static void expect_near(const char* case_name, const char* field,
	float actual, float expected, float tolerance) {
	bool pass = isfinite(actual) && fabsf(actual - expected) <= tolerance;
	fprintf(stderr, "%s %s %s actual=%.7f expected=%.7f tol=%.7f\n",
		case_name, pass ? "PASS" : "FAIL", field, actual, expected,
		tolerance);
	if(!pass)
		s_failures++;
}

static void expect_flag(const char* case_name, const char* field,
	bool actual, bool expected) {
	bool pass = actual == expected;
	fprintf(stderr, "%s %s %s actual=%d expected=%d\n",
		case_name, pass ? "PASS" : "FAIL", field, (int)actual,
		(int)expected);
	if(!pass)
		s_failures++;
}

static void move(struct entity* e, int axis, float delta,
	bool* collision_xz, bool* grounded) {
	struct AABB box = body_box();
	e->vel[axis] = delta;
	*collision_xz = false;
	*grounded = false;
	entity_try_move(e, e->pos, e->vel, &box, (size_t)axis,
		collision_xz, grounded);
}

int main(void) {
	test_fixture_dir("test_entity_swept_collision_tmp", s_fixture_dir,
		sizeof s_fixture_dir);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	MKDIR(s_fixture_dir);
	MKDIR(test_fixture_join(s_fixture_dir, "/region"));
	string_t level;
	string_init_set_str(level, s_fixture_dir);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242,
		true /* superflat */);
	s.scheduled_tick_budget = 0;
	for(int cx = -2; cx <= 2; cx++)
		for(int cz = -2; cz <= 2; cz++) {
			struct server_chunk* chunk = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &chunk)
				&& chunk);
		}
	clear_corridor();

	struct entity e;
	bool collision_xz, grounded;

	/* R1: destination [62,63.8] is clear; the swept body crosses
	 * stone [64,65]. Beta clips its feet at 65. */
	put(8, 64, 8, BLOCK_STONE, 0);
	reset_body(&e, 8.5F, 66.0F, 8.5F);
	move(&e, 1, -4.0F, &collision_xz, &grounded);
	expect_near("R1", "feet", e.pos[1], 65.0F, 1e-4F);
	expect_near("R1", "velocityY", e.vel[1], 0.0F, 1e-4F);
	expect_flag("R1", "grounded", grounded, true);
	put(8, 64, 8, BLOCK_AIR, 0);

	/* R2: lower slab metadata 0 has the real [64,64.5] collision box. */
	put(8, 64, 8, BLOCK_SLAB, 0);
	reset_body(&e, 8.5F, 65.6F, 8.5F);
	move(&e, 1, -3.6F, &collision_xz, &grounded);
	expect_near("R2", "feet", e.pos[1], 64.5F, 1e-4F);
	expect_near("R2", "velocityY", e.vel[1], 0.0F, 1e-4F);
	expect_flag("R2", "grounded", grounded, true);
	put(8, 64, 8, BLOCK_AIR, 0);

	/* R3: destination minX=11.6 is beyond the wall [10,11]. */
	put(10, 65, 8, BLOCK_STONE, 0);
	reset_body(&e, 8.9F, 65.0F, 8.5F);
	move(&e, 0, 3.0F, &collision_xz, &grounded);
	expect_near("R3", "centerX", e.pos[0], 9.7F, 1e-4F);
	expect_near("R3", "velocityX", e.vel[0], 0.0F, 1e-4F);
	expect_flag("R3", "collision_xz", collision_xz, true);
	put(10, 65, 8, BLOCK_AIR, 0);

	/* R4: destination overlaps the lower stone [56,57]. A bisection
	 * of only that hit skips the earlier stone [64,65]. */
	put(8, 64, 8, BLOCK_STONE, 0);
	put(8, 56, 8, BLOCK_STONE, 0);
	reset_body(&e, 8.5F, 66.0F, 8.5F);
	move(&e, 1, -10.0F, &collision_xz, &grounded);
	expect_near("R4", "feet", e.pos[1], 65.0F, 1e-4F);
	expect_near("R4", "velocityY", e.vel[1], 0.0F, 1e-4F);
	expect_flag("R4", "grounded", grounded, true);
	put(8, 56, 8, BLOCK_AIR, 0);

	/* R5a: a small downward move stays above the stone and is not grounded.
	 * entity_try_move_axis constructs the body and delegates to entity_try_move. */
	reset_body(&e, 8.5F, 65.3F, 8.5F);
	e.vel[1] = -0.2F;
	collision_xz = grounded = false;
	entity_try_move_axis(&e, 1, body_box, &collision_xz, &grounded);
	expect_near("R5a", "feet", e.pos[1], 65.1F, 1e-4F);
	expect_near("R5a", "velocityY", e.vel[1], -0.2F, 1e-4F);
	expect_flag("R5a", "grounded", grounded, false);
	put(8, 64, 8, BLOCK_AIR, 0);

	/* R5b: an already embedded wall permits motion deeper and then out;
	 * once outside, the wall clips an approach independently of the floor. */
	put(10, 65, 8, BLOCK_STONE, 0);
	reset_body(&e, 9.8F, 65.0F, 8.5F);
	move(&e, 0, 0.1F, &collision_xz, &grounded);
	expect_near("R5b", "embedded deeper X", e.pos[0], 9.9F, 1e-4F);
	expect_flag("R5b", "embedded deeper collision", collision_xz, false);
	move(&e, 0, -0.3F, &collision_xz, &grounded);
	expect_near("R5b", "embedded exit X", e.pos[0], 9.6F, 1e-4F);
	expect_flag("R5b", "embedded exit collision", collision_xz, false);
	move(&e, 0, 0.3F, &collision_xz, &grounded);
	expect_near("R5b", "wall approach X", e.pos[0], 9.7F, 0.011F);
	expect_flag("R5b", "wall approach collision", collision_xz, true);
	put(10, 65, 8, BLOCK_AIR, 0);

	/* R5c: an unrelated floor still blocks a downward move. */
	put(8, 64, 8, BLOCK_STONE, 0);
	reset_body(&e, 8.5F, 65.4F, 8.5F);
	move(&e, 1, -0.6F, &collision_xz, &grounded);
	expect_near("R5c", "floor feet", e.pos[1], 65.0F, 0.011F);
	expect_flag("R5c", "floor grounded", grounded, true);
	expect_near("R5c", "floor velocityY", e.vel[1], 0.0F, 1e-4F);

	/* ---- E cases (added with the fix): directions, ceilings, flush faces,
	 * zero moves, negative coordinates. Real stone, cleared corridors. */
	clear_corridor();

	/* E1 -X: wall [9,10]; center 12.0 (box [11.7,12.3]); dx -3.5 ends at
	 * [8.2,8.8], clear of the wall. Beta stops minX at 10 -> center 10.3. */
	put(9, 65, 8, BLOCK_STONE, 0);
	reset_body(&e, 12.0F, 65.0F, 8.5F);
	move(&e, 0, -3.5F, &collision_xz, &grounded);
	expect_near("E1", "centerX", e.pos[0], 10.3F, 1e-4F);
	expect_flag("E1", "collision_xz", collision_xz, true);
	expect_flag("E1", "no face penetration", e.pos[0] - 0.3F >= 10.0F, true);
	put(9, 65, 8, BLOCK_AIR, 0);

	/* E2 +Z: wall z [9,10]; center z 7.3 (box [7.0,7.6]); dz +3.0 ends at
	 * [10.0,10.6]. Beta stops maxZ at 9 -> center 8.7. */
	put(8, 65, 9, BLOCK_STONE, 0);
	reset_body(&e, 8.5F, 65.0F, 7.3F);
	move(&e, 2, 3.0F, &collision_xz, &grounded);
	expect_near("E2", "centerZ", e.pos[2], 8.7F, 1e-4F);
	expect_flag("E2", "collision_xz", collision_xz, true);
	expect_flag("E2", "no face penetration", e.pos[2] + 0.3F <= 9.0F, true);
	put(8, 65, 9, BLOCK_AIR, 0);

	/* E3 -Z: wall z [7,8]; center z 10.3 (box [10.0,10.6]); dz -3.8 ends at
	 * [6.2,6.8]. Beta stops minZ at 8 -> center 8.3. */
	put(8, 65, 7, BLOCK_STONE, 0);
	reset_body(&e, 8.5F, 65.0F, 10.3F);
	move(&e, 2, -3.8F, &collision_xz, &grounded);
	expect_near("E3", "centerZ", e.pos[2], 8.3F, 1e-4F);
	expect_flag("E3", "collision_xz", collision_xz, true);
	put(8, 65, 7, BLOCK_AIR, 0);

	/* E4 upward: ceiling [67,68]; feet 64 (top 65.8); dy +4.0 ends at
	 * [68,69.8], clear. Beta stops the top at 67 -> feet 65.2, not grounded. */
	put(8, 67, 8, BLOCK_STONE, 0);
	reset_body(&e, 8.5F, 64.0F, 8.5F);
	move(&e, 1, 4.0F, &collision_xz, &grounded);
	expect_near("E4", "feet", e.pos[1], 65.2F, 1e-4F);
	expect_near("E4", "velocityY", e.vel[1], 0.0F, 1e-4F);
	expect_flag("E4", "grounded", grounded, false);
	expect_flag("E4", "no face penetration", e.pos[1] + 1.8F <= 67.0F, true);
	put(8, 67, 8, BLOCK_AIR, 0);

	/* E5 flush face, repeated attempts: after clipping onto wall [10,11] the
	 * body must stay exactly at the face (no ghosting, no drift). */
	put(10, 65, 8, BLOCK_STONE, 0);
	reset_body(&e, 8.9F, 65.0F, 8.5F);
	move(&e, 0, 3.0F, &collision_xz, &grounded);
	float flush_x = e.pos[0];
	expect_flag("E5", "first clip at/before face", flush_x + 0.3F <= 10.0F, true);
	for(int i = 0; i < 3; i++) {
		move(&e, 0, 0.3F, &collision_xz, &grounded);
		expect_near("E5", "repeat X unchanged", e.pos[0], flush_x, 0.0F);
		expect_flag("E5", "repeat collision", collision_xz, true);
	}
	put(10, 65, 8, BLOCK_AIR, 0);

	/* E6 zero movement and face-touching floor: feet exactly on stone
	 * [64,65]. dx/dy 0 change nothing and set no flag; a 0.2 walk along the
	 * floor moves exactly 0.2 (the shared face never clips). */
	put(8, 64, 8, BLOCK_STONE, 0);
	put(9, 64, 8, BLOCK_STONE, 0);
	reset_body(&e, 8.5F, 65.0F, 8.5F);
	move(&e, 0, 0.0F, &collision_xz, &grounded);
	expect_near("E6", "zero dx X", e.pos[0], 8.5F, 0.0F);
	expect_flag("E6", "zero dx collision", collision_xz, false);
	move(&e, 1, 0.0F, &collision_xz, &grounded);
	expect_near("E6", "zero dy feet", e.pos[1], 65.0F, 0.0F);
	expect_flag("E6", "zero dy grounded", grounded, false);
	move(&e, 0, 0.2F, &collision_xz, &grounded);
	expect_near("E6", "walk on floor X", e.pos[0], 8.7F, 1e-6F);
	expect_flag("E6", "walk collision", collision_xz, false);
	put(8, 64, 8, BLOCK_AIR, 0);
	put(9, 64, 8, BLOCK_AIR, 0);

	/* E7 negative coordinates: wall x [-12,-11]; center -8.9 (box
	 * [-9.2,-8.6]); dx -4.0 ends at [-13.2,-12.6]. Beta stops minX at -11 ->
	 * center -10.7. */
	for(int x = -14; x <= -7; x++)
		for(int y = 60; y <= 69; y++)
			for(int z = 6; z <= 10; z++)
				put(x, y, z, BLOCK_AIR, 0);
	put(-12, 65, 8, BLOCK_STONE, 0);
	reset_body(&e, -8.9F, 65.0F, 8.5F);
	move(&e, 0, -4.0F, &collision_xz, &grounded);
	expect_near("E7", "centerX", e.pos[0], -10.7F, 1e-4F);
	expect_flag("E7", "collision_xz", collision_xz, true);
	expect_flag("E7", "no face penetration", e.pos[0] - 0.3F >= -11.0F, true);
	put(-12, 65, 8, BLOCK_AIR, 0);

	/* E8 origin cancellation: the swept face reaches wall x [0,1] from
	 * center -18 at +20. Adding the 17.7 gap back to -18 rounds beyond
	 * contact; bounded nextafter attempts used to freeze at the start. */
	for(int x = -20; x <= 3; x++)
		for(int y = 60; y <= 69; y++)
			for(int z = 6; z <= 10; z++)
				put(x, y, z, BLOCK_AIR, 0);
	put(0, 65, 8, BLOCK_STONE, 0);
	reset_body(&e, -18.0F, 65.0F, 8.5F);
	move(&e, 0, 20.0F, &collision_xz, &grounded);
	expect_near("E8", "centerX", e.pos[0], -0.3F, 1e-4F);
	expect_near("E8", "velocityX", e.vel[0], 0.0F, 1e-4F);
	expect_flag("E8", "collision_xz", collision_xz, true);
	expect_flag("E8", "no face penetration", e.pos[0] + 0.3F <= 0.0F,
		true);
	float e8_contact_x = e.pos[0];
	move(&e, 0, 0.3F, &collision_xz, &grounded);
	expect_near("E8", "repeat X unchanged", e.pos[0], e8_contact_x,
		0.0F);
	expect_flag("E8", "repeat collision", collision_xz, true);
	put(0, 65, 8, BLOCK_AIR, 0);

	/* G1: Beta Entity.moveEntity saves the ORIGINAL requested dy in var13
	 * (Entity.java:273), then sets onGround = var13 != var3 && var13 < 0
	 * (:417). A ceiling clip therefore clears prior grounded state. */
	clear_corridor();
	put(8, 67, 8, BLOCK_STONE, 0);
	reset_body(&e, 8.5F, 65.0F, 8.5F);
	e.vel[1] = 0.5F;
	collision_xz = false;
	grounded = true;
	struct AABB ground_box = body_box();
	entity_try_move(&e, e.pos, e.vel, &ground_box, 1, &collision_xz,
		&grounded);
	expect_near("G1", "feet", e.pos[1], 65.2F, 1e-4F);
	expect_near("G1", "velocityY", e.vel[1], 0.0F, 1e-4F);
	expect_flag("G1", "grounded", grounded, false);
	put(8, 67, 8, BLOCK_AIR, 0);

	/* G2: an upward move with no ceiling already clears prior grounded. */
	clear_corridor();
	reset_body(&e, 8.5F, 65.0F, 8.5F);
	e.vel[1] = 0.5F;
	collision_xz = false;
	grounded = true;
	ground_box = body_box();
	entity_try_move(&e, e.pos, e.vel, &ground_box, 1, &collision_xz,
		&grounded);
	expect_near("G2", "feet", e.pos[1], 65.5F, 1e-4F);
	expect_near("G2", "velocityY", e.vel[1], 0.5F, 1e-4F);
	expect_flag("G2", "grounded", grounded, false);

	/* G3: requested downward motion clipped by a real stone floor grounds. */
	clear_corridor();
	put(8, 64, 8, BLOCK_STONE, 0);
	reset_body(&e, 8.5F, 65.3F, 8.5F);
	e.vel[1] = -0.5F;
	collision_xz = false;
	grounded = false;
	ground_box = body_box();
	entity_try_move(&e, e.pos, e.vel, &ground_box, 1, &collision_xz,
		&grounded);
	expect_near("G3", "feet", e.pos[1], 65.0F, 1e-4F);
	expect_near("G3", "velocityY", e.vel[1], 0.0F, 1e-4F);
	expect_flag("G3", "grounded", grounded, true);
	put(8, 64, 8, BLOCK_AIR, 0);

	/* G4: an X wall clip retains the existing grounded state. */
	clear_corridor();
	put(10, 65, 8, BLOCK_STONE, 0);
	reset_body(&e, 8.9F, 65.0F, 8.5F);
	e.vel[0] = 3.0F;
	collision_xz = false;
	grounded = true;
	ground_box = body_box();
	entity_try_move(&e, e.pos, e.vel, &ground_box, 0, &collision_xz,
		&grounded);
	expect_near("G4", "centerX", e.pos[0], 9.7F, 1e-4F);
	expect_near("G4", "velocityX", e.vel[0], 0.0F, 1e-4F);
	expect_flag("G4", "collision_xz", collision_xz, true);
	expect_flag("G4", "grounded", grounded, true);
	put(10, 65, 8, BLOCK_AIR, 0);

	/* G5: feet already touch the floor. Original dy=-0.1 clips to zero;
	 * var13 remains negative, so Beta reports grounded even at zero travel. */
	clear_corridor();
	put(8, 64, 8, BLOCK_STONE, 0);
	reset_body(&e, 8.5F, 65.0F, 8.5F);
	e.vel[1] = -0.1F;
	collision_xz = false;
	grounded = false;
	ground_box = body_box();
	entity_try_move(&e, e.pos, e.vel, &ground_box, 1, &collision_xz,
		&grounded);
	expect_near("G5", "feet", e.pos[1], 65.0F, 0.0F);
	expect_near("G5", "velocityY", e.vel[1], 0.0F, 1e-4F);
	expect_flag("G5", "grounded", grounded, true);
	put(8, 64, 8, BLOCK_AIR, 0);

	s_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	test_fixture_dir_cleanup(s_fixture_dir);
	fprintf(stderr, "test_entity_swept_collision: failures=%d\n", s_failures);
	return s_failures ? 1 : 0;
}
