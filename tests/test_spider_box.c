/*
	Pins the Beta 1.7.3 spider COLLISION/INTERACTION box (audit #17a) and the
	ladder DESCENT clamp (audit #17b).

	Binds the SHARED source/entity/combat.h directly, so the byte-faithful Beta
	dimensions + the pure clamp helper tested here are the exact ones the engine
	uses (make_spider_bbox + getBoundingBox_creeper + the spider block in
	server_tick_monster, entity_monster.c) -- they cannot drift.

	Beta source:
	  - EntitySpider constructor: `this.setSize(1.4F, 0.9F);` -> the AABB is
	    width x height x width = 1.4 (X) x 0.9 (Y) x 1.4 (Z), centered on X/Z with
	    the FEET at posY (Entity.setSize stores w/h; Entity.setPosition builds the
	    box as [posX-w/2 .. posX+w/2] x [posY .. posY+h] x [posZ-w/2 .. posZ+w/2]).
	    mc173 corroborates byte-for-byte: LivingKind::Spider => (1.4, 0.9, 1.4).
	  - EntitySpider.isOnLadder() == isCollidedHorizontally, then
	    EntityLiving.moveEntityWithHeading: `if(motionY < -0.15D) motionY = -0.15D;`
	    -> a wall-pressed spider's descent is floored at -0.15/tick (slow slide).

	The AABB CONSTRUCTION itself (aabb_setsize_centered_offset) is exercised
	elsewhere; here we pin the DIMENSION constants and reproduce the exact
	feet-base, X/Z-centered extent arithmetic make_spider_bbox feeds it, so the
	box geometry is pinned without dragging aabb.c/blocks_data.h into this unit.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "entity/combat.h"

static bool feq(float a, float b) {
	return fabsf(a - b) < 1.0e-4f;
}

// Pin the Beta box dimensions: spider 1.4 wide x 0.9 tall x 1.4 deep, vs the
// 0.6-wide 0.6 baseline a biped uses. These are the exact values make_spider_bbox
// (entity_monster.c) passes to aabb_setsize_centered_offset.
static void test_box_dims(void) {
	// EntitySpider.setSize(1.4F, 0.9F).
	assert(feq(SPIDER_BBOX_WIDTH, 1.4f));
	assert(feq(SPIDER_BBOX_HEIGHT, 0.9f));
	// The box height reuses the existing SPIDER_BODY_HEIGHT (0.9), not a new value.
	assert(feq(SPIDER_BBOX_HEIGHT, SPIDER_BODY_HEIGHT));
	// The spider is WIDER and SHORTER than the 0.6x1.7 creeper/biped box: this is
	// the load-bearing gameplay difference (can't fit a 1-wide gap; fits a low
	// overhang). 1.4 > 0.6 and 0.9 < 1.7.
	assert(SPIDER_BBOX_WIDTH > 0.6f);
	assert(SPIDER_BBOX_HEIGHT < 1.7f);
}

// Reproduce the EXACT extents make_spider_bbox builds: a 1.4 x 0.9 x 1.4 box,
// X/Z centered on the origin, with the FEET at y=0 (oy = height/2 puts y in
// [0, height]). This mirrors aabb_setsize_centered_offset(out, 1.4, 0.9, 1.4,
// 0, 0.45, 0) so the geometry is pinned to the Beta box even though the AABB
// helper is linked separately.
static void test_box_extents(void) {
	const float sx = SPIDER_BBOX_WIDTH, sy = SPIDER_BBOX_HEIGHT,
				sz = SPIDER_BBOX_WIDTH;
	const float oy = sy * 0.5f; // feet-base offset (matches make_spider_bbox)

	float x1 = -sx * 0.5f + 0.0f, x2 = sx * 0.5f + 0.0f;
	float y1 = -sy * 0.5f + oy, y2 = sy * 0.5f + oy;
	float z1 = -sz * 0.5f + 0.0f, z2 = sz * 0.5f + 0.0f;

	// X spans [-0.7, 0.7] (half-extent 0.7 vs 0.3 for a 0.6-wide biped).
	assert(feq(x1, -0.7f));
	assert(feq(x2, 0.7f));
	assert(feq(x2 - x1, 1.4f));
	// Z identical to X (the AABB is width x height x WIDTH).
	assert(feq(z1, -0.7f));
	assert(feq(z2, 0.7f));
	assert(feq(z2 - z1, 1.4f));
	// Y: feet AT THE FEET (y1 == 0), top at 0.9 (NOT Y-centered) -- the eye height
	// MOB_EYE_HEIGHT_SPIDER (0.765) sits just below the 0.9 box top.
	assert(feq(y1, 0.0f));
	assert(feq(y2, 0.9f));
	assert(feq(y2 - y1, 0.9f));
	assert(MOB_EYE_HEIGHT_SPIDER < y2); // 0.765 < 0.9: eye inside the box
}

// Pin that the spider's MELEE vertical box height tracks its 0.9 collision box
// (audit #17a follow-up), not the 1.7 biped box -- so all THREE boxes (collision,
// raycast, melee) agree. mob_body_height(id) is the single per-id source the melee
// overlap test now consumes.
static void test_melee_box_height(void) {
	// Spider melee box height == its 0.9 collision box height (NOT the 1.7 biped).
	assert(feq(mob_body_height(MONSTER_SPIDER), SPIDER_BBOX_HEIGHT)); // 0.9
	assert(feq(mob_body_height(MONSTER_SPIDER), 0.9f));
	// Every other hostile keeps the 1.7 biped box height.
	assert(feq(mob_body_height(MONSTER_ZOMBIE), MOB_BODY_HEIGHT));   // 1.8
	assert(feq(mob_body_height(MONSTER_SKELETON), MOB_BODY_HEIGHT));
	assert(feq(mob_body_height(MONSTER_CREEPER), MOB_BODY_HEIGHT));
	assert(mob_body_height(MONSTER_SPIDER) < mob_body_height(MONSTER_ZOMBIE));

	// The squat box means the spider's bite reaches LESS HIGH than a biped's. A player
	// 1.0 block above the mob's feet: inside the 1.7 biped box, OUTSIDE the 0.9 spider
	// box -- the exact phantom-reach the per-id height removes.
	const float p_feet = 65.0f, m_feet = 64.0f; // player 1.0 above the mob
	assert(melee_y_overlap(p_feet, m_feet, mob_body_height(MONSTER_ZOMBIE)));  // biped: bite
	assert(!melee_y_overlap(p_feet, m_feet, mob_body_height(MONSTER_SPIDER))); // spider: no bite
}

// Pin the ladder descent clamp value + the pure spider_clamp_fall helper. Beta
// `if(motionY < -0.15) motionY = -0.15` floors ONLY downward motion.
static void test_fall_clamp(void) {
	// The exact Beta clamp magnitude.
	assert(feq(SPIDER_FALL_CLAMP, 0.15f));

	// A fast fall is floored to exactly -0.15 (slow slide, not free-fall).
	assert(feq(spider_clamp_fall(-2.0f), -SPIDER_FALL_CLAMP));
	assert(feq(spider_clamp_fall(-0.15001f), -SPIDER_FALL_CLAMP));
	// A descent already at/above the floor is UNCHANGED.
	assert(feq(spider_clamp_fall(-0.15f), -0.15f));
	assert(feq(spider_clamp_fall(-0.1f), -0.1f));
	assert(feq(spider_clamp_fall(0.0f), 0.0f));
	// UPWARD motion is never touched -- the clamp floors descent only, so a spider
	// that still has residual climb/leap/knockback rise this tick keeps it.
	assert(feq(spider_clamp_fall(0.2f), 0.2f));
	assert(feq(spider_clamp_fall(SPIDER_CLIMB_VEL_Y), SPIDER_CLIMB_VEL_Y)); // 0.2
	assert(feq(spider_clamp_fall(SPIDER_LEAP_VEL_Y), SPIDER_LEAP_VEL_Y));   // 0.4

	// Climb (+0.2 up) and clamp (floor at -0.15 down) are disjoint, never equal:
	// the climb branch and the clamp branch are mutually exclusive in the engine,
	// and even their values can't collide (one is strictly positive, one negative).
	assert(SPIDER_CLIMB_VEL_Y > 0.0f);
	assert(-SPIDER_FALL_CLAMP < 0.0f);
}

int main(void) {
	test_box_dims();
	test_box_extents();
	test_melee_box_height();
	test_fall_clamp();
	printf("test_spider_box: spider box pinned (%.1f wide x %.1f tall, feet-base; "
		   "ladder descent floor -%.2f, climb +%.1f)\n",
		   SPIDER_BBOX_WIDTH, SPIDER_BBOX_HEIGHT, SPIDER_FALL_CLAMP,
		   SPIDER_CLIMB_VEL_Y);
	return 0;
}
