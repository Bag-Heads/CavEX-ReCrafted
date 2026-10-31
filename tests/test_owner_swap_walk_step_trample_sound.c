/* Copyright (c) 2026 CaveWii_v2 contributors
 * split-dimensions-plan-0924/A1d, P2 inventory row #16: movement footsteps
 * and farmland trampling, owner-swapped.
 *
 * Beta citations (client Jad decompile,
 * ../mcb173-retromcp/minecraft/src_original/net/minecraft/src/):
 *   - Entity.moveEntity, Entity.java:436-457: when canTriggerWalking(),
 *     not sneaking on the ground, and ridingEntity == null, the horizontal travel accumulates
 *     `distanceWalkedModified += sqrt(dx*dx + dz*dz) * 0.6D` (:438); when it
 *     crosses `nextStepDistance` with a solid block underfoot (a non-air id below,
 *     :447) `++nextStepDistance` (:448), the step sound plays (:449-455) and
 *     that block's onEntityWalking(worldObj, x, y, z, entity)
 *     runs for THIS entity (:457). Every entity that moves gets its own
 *     odometer -- a second player is a second walker.
 *   - Entity.java:40-42: `distanceWalkedModified = 0.0F`,
 *     `nextStepDistance = 1` -- per entity.
 *   - BlockFarmland.onEntityWalking, BlockFarmland.java:46-50: a
 *     World.rand.nextInt(4) == 0 turns the cell to dirt with notify --
 *     a 1-in-4 trample per footstep, for any walker.
 *   - EntityLiving.onLivingUpdate, EntityLiving.java:618-622 / 642: sneaking
 *     while on the ground is the sneak-grounded (isSneaking && onGround) exclusion.
 *
 * CaveWii today: the host's SRPC_PLAYER_POS handler (server_local.c) runs the
 * odometer, the footstep sound (server_local_emit_footstep) and the farmland
 * trample per accepted sample; server_local_guest_position -- the guest's
 * ONLY movement route -- does none of them (its own header says so: "still
 * belong to the primary SRPC_PLAYER_POS path"). RED on the base.
 *
 * Two ctest names, one executable (argv[1]):
 *   base  -- host reference: odometer, ONE footstep sound on the crossing,
 *            the sneak exclusion, and a farmland trample under the feet.
 *   red   -- the same walk at the guest. WILL_FAIL until wired. */
#include "support/owner_swap_harness.h"
#include "platform/audio.h"

#define TEST_NAME "test_owner_swap_walk_step_trample_sound"

/* Four 0.5-block steps: 0.3 each on the odometer, 1.2 > nextStepDistance 1
 * on the fourth -> exactly one footstep with grass underfoot. */
static void walk_four(struct server_local* s, struct server_player* p) {
	for(int i = 0; i < 4; i++)
		walk_step(s, p, 0.5, false);
}
/* Shuttle over the farmland cell at column `fx` (feet inside [fx, fx+1)) until
 * it tramples or the attempt budget runs out. Each footstep has a 1-in-4
 * trample chance, and a footstep fires every ~3.3 samples, so 240 samples are
 * ~72 footsteps: P(no trample) = 0.75^72 ~ 1e-9. Returns the sample count. */
static int shuttle_until_trampled(struct server_local* s, struct server_player* p,
	int fx) {
	int n = 0;
	for(; n < 240 && block_at(s, fx, 3, 0) == BLOCK_FARMLAND; n++) {
		double dx = p->x < (double)fx + 0.5 ? 0.5 : -0.5;
		walk_step(s, p, dx, false);
	}
	return n;
}
static void walk_owner(struct server_local* s, struct server_player* p,
	const char* who, bool red) {
	const char* tag = red ? "  [row #16: RED on base]" : "";
	char label[256];
	int fx = (int)floor(p->x);
	assert(block_at(s, fx, 3, 0) == BLOCK_GRASS);
	assert(block_at(s, fx + 2, 3, 0) == BLOCK_GRASS);
	p->distance_walked = 0.0F;
	p->next_step_distance = 1;
	counters_reset();

	/* Odometer + the ONE step sound on the crossing. */
	walk_four(s, p);
	snprintf(label, sizeof(label),
		"%s: four 0.5-block steps accumulate 1.2 on the odometer "
		"(Entity.moveEntity:438, *0.6)%s", who, tag);
	check(fabsf(p->distance_walked - 1.2F) < 1e-5F, label);
	snprintf(label, sizeof(label),
		"%s: nextStepDistance advanced to 2 on the crossing (Entity.moveEntity:448)%s",
		who, tag);
	check(p->next_step_distance == 2, label);
	snprintf(label, sizeof(label),
		"%s: exactly ONE positional step sound was published for the crossing "
		"(Entity.moveEntity:449-455), at the feet%s", who, tag);
	check(sound_msgs == 1 && last_sound_id != AUDIO_SOUND_NONE
		&& fabsf(last_sound_pos[1] - (float)(p->y - PLAYER_EYE_HEIGHT)) < 1e-3F,
		label);

	/* Sneaking on the ground: excluded from the odometer (var18). The host
	 * reads Beta's `isSneaking && onGround` from the body's PERSISTENT state
	 * as it stood BEFORE the sample (server_local.c: "use the persistent
	 * pre-call onGround/isSneaking snapshot"), so the first sneaking sample
	 * still accumulates and only the NEXT one is excluded. */
	walk_step(s, p, 0.5, true); /* arms sneaking on the body */
	float walked = p->distance_walked;
	walk_step(s, p, 0.5, true);
	snprintf(label, sizeof(label),
		"%s: a step taken while sneaking on the ground adds nothing "
		"(EntityLiving isSneaking && onGround exclusion)", who);
	check(p->distance_walked == walked, label);
	walk_step(s, p, 0.0, false); /* un-sneak in place */
	walk_step(s, p, 0.0, false); /* ...and let the cleared flag take effect */

	/* Farmland trample under the feet (BlockFarmland.onEntityWalking:46-50).
	 * The approach step onto the cell is itself a candidate footstep, so the
	 * counters start before it. */
	int tx = (int)floor(p->x) + 1;
	put_block(s, tx, 3, 0, BLOCK_FARMLAND, 0);
	counters_reset();
	walk_step(s, p, ((double)tx + 0.25) - p->x, false);
	assert((int)floor(p->x) == tx);
	int samples = 1 + shuttle_until_trampled(s, p, tx);
	snprintf(label, sizeof(label),
		"%s: shuttling over farmland tramples it to dirt within the budget "
		"(1-in-4 per footstep; %d samples used)%s", who, samples, tag);
	check(block_at(s, tx, 3, 0) == BLOCK_DIRT, label);
	snprintf(label, sizeof(label),
		"%s: those footsteps published step sounds too%s", who, tag);
	check(sound_msgs >= 1, label);
	put_block(s, tx, 3, 0, BLOCK_GRASS, 0);
}

/* ======================================================================== */
static void case_base(void) {
	struct server_local* s = admitted_fixture();
	body_stand_at(s->guest_actor, 12.5, 0.5); /* out of the host's lane */
	walk_owner(s, &s->player, "host", false);
	check(s->guest_actor->distance_walked == 0.0F,
		"guest: the host's walk never touched the guest's odometer");
	teardown(s);
}

/* ======================================================================== */
static void case_red(void) {
	struct server_local* s = admitted_fixture();
	struct server_player* h = &s->player;
	struct server_player* g = s->guest_actor;
	body_stand_at(g, 8.5, 0.5);
	float host_walked = h->distance_walked;
	walk_owner(s, g, "guest", true);
	check(h->distance_walked == host_walked,
		"host: the guest's walk never touched the host's odometer");
	teardown(s);
}

/* ======================================================================== */
int main(int argc, char** argv) {
	const char* which = argc > 1 ? argv[1] : NULL;
	if(!which || (strcmp(which, "base") && strcmp(which, "red"))) {
		fprintf(stderr, "usage: %s base|red  (one case per process)\n", argv[0]);
		return 2;
	}
	harness_init();
	if(!strcmp(which, "base"))
		case_base();
	else
		case_red();
	return harness_result(TEST_NAME, which);
}
