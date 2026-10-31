/* Copyright (c) 2026 CaveWii_v2 contributors
 * next-gaps-0924/GOLDEN-0925: focused assertions for the FIRST divergent
 * fields of the five whole-sim goldens that the W2-MOVE parity commits moved
 * (Astra kickoff ruling 8, 2026-09-25: re-capture only after each divergence
 * is proven to be the intended Beta correction, with an assertion that pins
 * the corrected behaviour INDEPENDENTLY of the frozen traces).
 *
 * Each section re-creates the exact first-divergence scene that the bisect
 * dumps recorded (tmp/jobs/next-gaps-0924/golden-dump/, GOLDEN-result.md) --
 * the same superflat fixture world the goldens run in, the same entity kinds,
 * the recorded positions and velocities -- drives the ONE Beta rule that
 * moved the field through the production entry point, and compares the
 * result against a transliteration of the Beta 1.7.3 decompile written here
 * (mcb173-retromcp src_original), bit-exact where the port stores the value
 * exactly as Beta computes it, and against the values the divergent dumps
 * recorded (printed to 6 decimals there, hence the 1e-6 tolerance on those).
 *
 *   S1  test_split_off_equiv: entities component, first divergent row = the
 *       chickens id 7/8 at server tick 84 (seed 12345 superflat): Entity.
 *       applyEntityCollision [Entity.java:668-691] from EntityLiving.
 *       onLivingUpdate:641-651 -- mob 8's loop pushes 7 after 8's own move;
 *       7's earlier loop found 8 still 0.5154 away and the strict
 *       intersectsWith [AxisAlignedBB.java:185] gate (0.15+0.15+0.2 = 0.5)
 *       excluded it. Cause commit aa8bfa1c.
 *   S2  test_host_damage_golden + test_block_hooks_actor_golden: RPC-ORDER
 *       digest, first divergent row = sheep id 2/3 spawned as one pack 1.0
 *       apart in Z at server tick 60, pushed at tick 61: sheep 2 moves, its
 *       loop reaches sheep 3 still at its spawn cell, abs_max(dx,dz) =
 *       1.089981 = |dz| (Z dominant, var8 = 1/sqrt unclamped), so the Z
 *       half is the full 0.05F and X carries dx/|dz| * 0.05F; sheep
 *       3's own loop then finds 2 1.269 away (>= 1.1) and pushes nothing.
 *       The extra CRPC_ENTITY_MOVE rows follow from the moved sheep. Cause
 *       commit aa8bfa1c.
 *   S3  test_column_dedup_state_equiv (seed 12345 walk, tick 241): squids id
 *       5/6 spawned as a pack 1.0 apart in Z; 5's loop pushes 6 before 6's
 *       move (abs_max 1.0205 = |dz| >= 1: Z half the full 0.05F), 6's loop
 *       pushes 5 after 6's move (abs_max 0.8454 < 1: var8 = 1/sqrt > 1 clamps
 *       to 1, both halves scale by 1/sqrt only). Both impulses land
 *       on 5 (repeated-call semantics); the frozen client trace then sees
 *       ENTITY_MOVE boundaries where the base squids were at rest. Cause
 *       commit aa8bfa1c.
 *
 *   S4  all five goldens, second stage (commit b0931e6e, "clip entity movement
 *       against swept block faces", docs/intent/move-swept-collision.md):
 *       Entity.moveEntity:300-328 + AxisAlignedBB.calculateX/Y/ZOffset:104-195
 *       land a mover EXACTLY on the limiting block face. First divergent rows:
 *       sheep 1's landing y 4.004537 -> 4.000000 (split_off tick 20 / host
 *       step 15 / hooks step 5 CRPC_ENTITY_MOVE payloads) and the walk item
 *       1's x 88.871231 -> 88.875000 against the face at 89 (column_dedup
 *       seed 12345, walk tick 60). The old destination-only 0.01 bisection
 *       left bodies a hair off the face.
 *   S5  test_column_dedup_state_equiv / _render_equiv, first stage (commit
 *       d8b3595d): World.handleMaterialAcceleration:1418-1450 +
 *       BlockFluid.getFlowVector:86-166 through EntityItem.handleWaterMovement:
 *       79-81 (the PLAIN box). First divergent row = the dropped item 1 at
 *       (88.37, 46.55, 19.48) of the seed-12345 natural world, walk tick 1:
 *       its velocity gains 0.014D along the normalized (+X, -Z) current before
 *       EntityItem's 0.98 drag. The same current is rebuilt here cell by cell
 *       from getFlowVector's decay rule (the walk's water had already flowed
 *       and dropped the item before tick 1, so a fresh generation cannot
 *       reproduce it as-is).
 *   S6  test_column_dedup_state_equiv, the first HASH-visible d8b3595d field
 *       (server tick 568 -> 569, the ids-plane order of two far lava random
 *       ticks): skeleton 17's Entity.handleWaterMovement box [Entity.java:
 *       543-545] reaches water in column 46 while its foot cell is air, so
 *       inWater flips wet -> the port's swim roll draws once more from the
 *       shared rand_src -> the same tick's random-tick sweep shifts one draw.
 *       Pins the verdict (Beta transliteration vs production, bit-exact push)
 *       at ticks 569 (wet) and 568 (dry).
 *
 * The scenes never run a mob AI tick, so no RNG stream is consumed: what is
 * under test is the one Beta rule, exactly as the goldens' first divergent
 * fields exercised it.
 */
#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "block/blocks.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/monsters_object.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/audio.h"
#include "platform/thread.h"
#include "support/test_fixture_dir.h"
#include "support/mob_motion_fixture.h"
#include <m-lib/m-string.h>

static int s_checks, s_failures, s_section_start;

static void fail(const char* what, const char* msg) {
	s_failures++;
	fprintf(stderr, "FAIL %s: %s\n", what, msg);
}

static void check(const char* what, bool ok, const char* msg) {
	s_checks++;
	if(!ok)
		fail(what, msg);
}

static uint32_t fbits(float f) {
	uint32_t u;
	memcpy(&u, &f, sizeof u);
	return u;
}

/* The port stores velocity as float; Beta computes the sum in double and the
 * port rounds it once on the store. Bit-exact against that same expression. */
static void bits_or_die(const char* what, float got, double want) {
	s_checks++;
	if(fbits(got) != fbits((float)want)) {
		char msg[200];
		snprintf(msg, sizeof msg, "got %.9g (%08x) want %.9g (%08x)", got,
				 fbits(got), (float)want, fbits((float)want));
		fail(what, msg);
	}
}

static void near_or_die(const char* what, double got, double want,
						double tol) {
	s_checks++;
	if(fabs(got - want) > tol) {
		char msg[200];
		snprintf(msg, sizeof msg, "got %.9f want %.9f (|d|=%.3g > %.1g)", got,
				 want, fabs(got - want), tol);
		fail(what, msg);
	}
}

static void section_done(const char* msg) {
	printf("%s %s\n", s_failures == s_section_start ? "PASS" : "RED ", msg);
	fflush(stdout);
	s_section_start = s_failures;
}

/* ---- Beta 1.7.3 transliterations (the spec, written independently of
 * source/entity/entity.c) ------------------------------------------------- */

/* MathHelper.abs_max [MathHelper.java:36-46]. */
static double beta_abs_max(double var0, double var2) {
	if(var0 < 0.0)
		var0 = -var0;
	if(var2 < 0.0)
		var2 = -var2;
	return var0 > var2 ? var0 : var2;
}

/* Entity.applyEntityCollision [Entity.java:668-691], `this` = the pushed
 * entity, var1 = the mover. Returns false when the 0.01F gate skips it.
 * this.addVelocity(-var2, 0, -var4); var1.addVelocity(var2, 0, var4). */
static bool beta_apply_entity_collision(double this_x, double this_z,
										double var1_x, double var1_z,
										double* var2_out, double* var4_out) {
	double var2 = var1_x - this_x;
	double var4 = var1_z - this_z;
	double var6 = beta_abs_max(var2, var4);
	if(!(var6 >= (double)0.01F))
		return false;
	var6 = (double)(float)sqrt(var6); /* MathHelper.sqrt_double:18-20 */
	var2 /= var6;
	var4 /= var6;
	double var8 = 1.0 / var6;
	if(var8 > 1.0)
		var8 = 1.0;
	var2 *= var8;
	var4 *= var8;
	var2 *= (double)0.05F;
	var4 *= (double)0.05F;
	var2 *= (double)(1.0F - 0.0F); /* entityCollisionReduction, Entity.java:49 */
	var4 *= (double)(1.0F - 0.0F);
	*var2_out = var2;
	*var4_out = var4;
	return true;
}

/* EntityLiving.onLivingUpdate:641-651 candidate gate for two same-height
 * bodies: boundingBox.expand(0.2F, 0, 0.2F).intersectsWith(other) strict
 * [AxisAlignedBB.java:185] -- |dx| < wa/2 + wb/2 + 0.2 on BOTH axes. */
static bool beta_push_candidate(double mover_x, double mover_z, float mover_w,
								double other_x, double other_z, float other_w) {
	const double hm = (double)(mover_w / 2.0F), ho = (double)(other_w / 2.0F);
	const double ex = (double)0.2F;
	return other_x + ho > mover_x - hm - ex && other_x - ho < mover_x + hm + ex
		&& other_z + ho > mover_z - hm - ex && other_z - ho < mover_z + hm + ex;
}

/* ---- the goldens' fixture world ------------------------------------------ */
static char s_path[128];

static struct server_local* fixture(void) {
	test_fixture_dir("test_golden_move_divergence", s_path, sizeof(s_path));
#ifdef _WIN32
	_mkdir(s_path);
#else
	mkdir(s_path, 0777);
#endif
	struct server_local* s = calloc(1, sizeof(*s));
	assert(s);
	/* Same creation as tests/support/split_residency_fixture.h: superflat
	 * (bedrock 0, dirt 1-2, grass 3), seed 12345 -- the goldens' world. */
	server_local_test_create_without_thread(s, false, false, false, false,
											false, true, false);
	string_t path;
	string_init_set_str(path, s_path);
	assert(level_archive_create_new(&s->level, path, 12345, (vec3) {0, 4, 0}));
	assert(server_world_create_with_populate_scratch(
		&s->worlds[0], path, WORLD_DIM_OVERWORLD, 12345, true,
		s->world_populate_scratch));
	string_clear(path);
	s->worlds[0].session = s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s->worlds[0].local = s; /* the goldens' one-dimension superflat: worlds[0] is the Overworld */
	s->rand_src = (struct random_gen) {.seed = 24680};
	s->player.has_pos = false; /* no host body in the push query */
	s->autosave_ticks = 0;
	for(int cz = -3; cz <= 4; cz++)
		for(int cx = -3; cx <= 3; cx++) {
			struct server_chunk* c = NULL;
			for(unsigned n = 0; n < 1000 && !c; n++) {
				server_world_begin_update(&s->worlds[0]);
				if(!server_world_ensure_populated(&s->worlds[0], s, cx, cz, &c))
					c = NULL;
			}
			assert(c);
		}
	return s;
}

static void fixture_destroy(struct server_local* s) {
	server_world_discard(&s->worlds[0]);
	level_archive_discard(&s->level);
	server_world_populate_scratch_destroy(s->world_populate_scratch);
	inventory_destroy(&s->player.inventory, &s->player);
	dict_entity_clear(s->entities);
	dict_autosave_saved_clear(s->autosave_saved_this_epoch);
	string_clear(s->level_name);
	if(s->residency)
		mem2_free(s->residency);
	free(s);
	test_fixture_dir_cleanup(s_path);
}

/* A living mob of `kind` placed at the recorded position with the recorded
 * velocity -- an authoritative position mutation, so the section-list query
 * membership is refreshed exactly as the production movers do. */
static struct entity* mob_at(struct server_local* s, int kind, double x,
							 double y, double z, float vx, float vz) {
	struct entity* m = server_local_spawn_monster((vec3) {(float)x, (float)y,
														 (float)z}, kind, s);
	assert(m && m->on_server && m->type == ENTITY_MONSTER
		   && m->data.monster.id == kind);
	m->pos[0] = (float)x;
	m->pos[1] = (float)y;
	m->pos[2] = (float)z;
	m->vel[0] = vx;
	m->vel[1] = 0.0F;
	m->vel[2] = vz;
	test_mob_sync(m); /* motion units S2: the fixture pins the doubles */
	server_local_refresh_entity_membership(s, m);
	return m;
}

static void remove_mob(struct server_local* s, struct entity* m) {
	dict_entity_erase(s->entities, m->id);
	free(m); /* fixture-owned scene body; nothing else references it */
}

/* One push scene: `mover` has just finished its move at its current position,
 * `pushed` sits where the mover's loop finds it. Runs the production loop for
 * the mover only and checks both bodies against the Beta transliteration and
 * the recorded dump values (6-decimal). want_* are the dump's post-push rows;
 * pass NAN to skip a recorded comparison. */
struct push_expect {
	double pushed_vx, pushed_vz, mover_vx, mover_vz;
};

static void scene_push(const char* tag, struct server_local* s,
					   struct entity* mover, struct entity* pushed,
					   bool expect_push, const struct push_expect* want) {
	const float mover_vx0 = mover->vel[0], mover_vz0 = mover->vel[2];
	const float pushed_vx0 = pushed->vel[0], pushed_vz0 = pushed->vel[2];
	/* Mob motion units S2: a mob's motion is the authoritative double; Beta
	 * adds each push to motionX in double (Entity.addVelocity), so the
	 * expectation starts from that double, and the float mirror is compared
	 * as its single rounding. */
	const double mover_mx0 = mover->data.monster.motion_d.motion[0];
	const double mover_mz0 = mover->data.monster.motion_d.motion[2];
	const double pushed_mx0 = pushed->data.monster.motion_d.motion[0];
	const double pushed_mz0 = pushed->data.monster.motion_d.motion[2];
	const double mx = (double)mover->pos[0], mz = (double)mover->pos[2];
	const double px = (double)pushed->pos[0], pz = (double)pushed->pos[2];

	bool candidate = beta_push_candidate(mx, mz, mover->width, px, pz,
										 pushed->width);
	char msg[200];
	snprintf(msg, sizeof msg, "strict expand(0.2) gate: candidate=%d expected=%d",
			 candidate, expect_push);
	check(tag, candidate == expect_push, msg);

	entity_living_collide_with_nearby(mover, s);

	if(!expect_push) {
		check(tag, fbits(pushed->vel[0]) == fbits(pushed_vx0)
				   && fbits(pushed->vel[2]) == fbits(pushed_vz0),
			  "no push expected: pushed velocity changed");
		check(tag, fbits(mover->vel[0]) == fbits(mover_vx0)
				   && fbits(mover->vel[2]) == fbits(mover_vz0),
			  "no push expected: mover velocity changed");
		return;
	}
	double var2, var4;
	check(tag, beta_apply_entity_collision(px, pz, mx, mz, &var2, &var4),
		  "Beta 0.01F gate should admit this pair");
	/* this (pushed) += (-var2, -var4); var1 (mover) += (var2, var4). */
	bits_or_die(tag, pushed->vel[0], pushed_mx0 - var2);
	bits_or_die(tag, pushed->vel[2], pushed_mz0 - var4);
	bits_or_die(tag, mover->vel[0], mover_mx0 + var2);
	bits_or_die(tag, mover->vel[2], mover_mz0 + var4);
	check(tag, pushed->data.monster.motion_d.motion[0] == pushed_mx0 - var2
			   && pushed->data.monster.motion_d.motion[2] == pushed_mz0 - var4
			   && mover->data.monster.motion_d.motion[0] == mover_mx0 + var2
			   && mover->data.monster.motion_d.motion[2] == mover_mz0 + var4,
		  "the double motion takes Beta's exact addVelocity sums");
	if(want) {
		near_or_die(tag, pushed->vel[0], want->pushed_vx, 1e-6);
		near_or_die(tag, pushed->vel[2], want->pushed_vz, 1e-6);
		near_or_die(tag, mover->vel[0], want->mover_vx, 1e-6);
		near_or_die(tag, mover->vel[2], want->mover_vz, 1e-6);
	}
}

/* ---- S1: test_split_off_equiv, server tick 84, chickens 7 and 8 ---------- */
static void s1_split_off_chickens(struct server_local* s) {
	/* Dump rows (tick 83 -> 84, seed 12345): 7 moved from 10.607676 to
	 * 10.654360; 8 from 11.169750 to 11.026567; z 7 -25.515703, 8 -25.440016.
	 * Velocities after friction and before the push: 7 (0.025490, -0.002593),
	 * 8 (-0.078178, 0.009905); after: 7 (-0.005015, -0.008796),
	 * 8 (-0.047674, 0.016108). */
	struct entity* c7 = mob_at(s, MONSTER_CHICKEN, 10.654360, 4.002582,
							   -25.515703, 0.025490F, -0.002593F);
	struct entity* c8 = mob_at(s, MONSTER_CHICKEN, 11.169750, 4.006503,
							   -25.440016, -0.078178F, 0.009905F);
	check("S1 chicken size", c7->width == 0.3F && c8->width == 0.3F,
		  "EntityChicken.setSize(0.3F, 0.4F)");
	check("S1 id order", c7->id < c8->id, "7 ticks before 8 (id = tick order)");

	/* 7's loop, run after 7's own move: 8 has not moved yet -- 0.515390 apart,
	 * the strict gate needs < 0.5. Nothing happens. */
	scene_push("S1 7's loop (8 at 11.169750)", s, c7, c8, false, NULL);

	/* 8 moves, then 8's loop reaches 7 at 0.372207. */
	c8->pos[0] = 11.026567F;
	test_mob_sync(c8);
	server_local_refresh_entity_membership(s, c8);
	struct push_expect want = {.pushed_vx = -0.005015, .pushed_vz = -0.008796,
							   .mover_vx = -0.047674, .mover_vz = 0.016108};
	scene_push("S1 8's loop pushes 7", s, c8, c7, true, &want);
	remove_mob(s, c7);
	remove_mob(s, c8);
	section_done("S1 split_off: chickens 7/8 at tick 84 -- one Beta impulse, "
				 "7's loop gated by strict expand(0.2) intersectsWith");
}

/* ---- S2: host_damage / block_hooks, server tick 61, sheep 2 and 3 --------- */
static void s2_spawn_pack_sheep(struct server_local* s, double ox, double oz,
								const char* which) {
	/* Both fixtures spawn the pack at block centres 1.0 apart in Z; the
	 * block_hooks rows are the host_damage rows translated by (-3, 0, -1)
	 * (its far-column stream differs), so the same scene runs at both origins.
	 * Dump rows at tick 61: 2 moved to (ox-16.356760, oz+54.589981), 3 still at
	 * its spawn (ox-16.5, oz+53.5); 2's post-friction velocity (0.130349,
	 * 0.081883) -> (0.136920, 0.131883): the full +0.05F on Z. */
	struct entity* sh2 = mob_at(s, MONSTER_SHEEP, ox - 16.356760, 4.000113,
								oz + 54.589981, 0.130349F, 0.081883F);
	struct entity* sh3 = mob_at(s, MONSTER_SHEEP, ox - 16.5, 4.0, oz + 53.5,
								0.027779F, -0.188678F);
	check("S2 sheep size", sh2->width == 0.9F && sh3->width == 0.9F,
		  "EntitySheep.setSize(0.9F, 1.3F)");
	check("S2 id order", sh2->id < sh3->id, "2 ticks before 3");
	char tag[96];
	snprintf(tag, sizeof tag, "S2 %s 2's loop pushes 3 at spawn", which);
	struct push_expect want = {.pushed_vx = 0.027779 - 0.006571,
							   .pushed_vz = -0.188678 - 0.05,
							   .mover_vx = 0.136920, .mover_vz = 0.131883};
	scene_push(tag, s, sh2, sh3, true, &want);
	/* The Z half is 0.05F to rounding: Z is the dominant axis (abs_max =
	 * |var4| = 1.089981 >= 1, var8 = 1/sqrt unclamped), so var4 / sqrt / sqrt
	 * == var4 / abs_max == 1 before the 0.05F scale. */
	double var2, var4;
	assert(beta_apply_entity_collision(ox - 16.5, oz + 53.5, ox - 16.356760,
									   oz + 54.589981, &var2, &var4));
	check(tag, fabs(var4 - (double)0.05F) < 2e-8,
		  "Z half must be 0.05F (dominant axis; sqrt_double is a FLOAT sqrt, "
		  "so var4/var6/var6 == 1 only to ~1e-7 relative)");
	near_or_die(tag, var2, 0.0065707, 1e-6);

	/* 3 then moves (its move consumed the impulse) and runs its own loop: 2 is
	 * 1.269276 away in Z, the gate needs < 0.45 + 0.45 + 0.2 = 1.1. Nothing. */
	sh3->pos[0] = (float)(ox - 16.484068);
	sh3->pos[2] = (float)(oz + 53.320705);
	test_mob_sync(sh3);
	server_local_refresh_entity_membership(s, sh3);
	snprintf(tag, sizeof tag, "S2 %s 3's loop (2 at 1.269 in Z)", which);
	scene_push(tag, s, sh3, sh2, false, NULL);
	remove_mob(s, sh2);
	remove_mob(s, sh3);
}

static void s2_spawn_pack(struct server_local* s) {
	s2_spawn_pack_sheep(s, 0.0, 0.0, "host_damage");
	s2_spawn_pack_sheep(s, -3.0, -1.0, "block_hooks");
	section_done("S2 host_damage/block_hooks: spawn-pack sheep 2/3 at tick 61 "
				 "-- exact 0.05F Z half, no reverse push");
}

/* ---- S3: column_dedup_state, seed 12345 walk tick 236 (server 241) -------- */
static void s3_squid_pack(struct server_local* s) {
	/* Squids 5/6 spawned as a pack at (14.5, 40, 66.5) / (14.5, 40, 67.5).
	 * Tick 241: 5 moves to (14.603788, 66.479485), its loop pushes 6 (still at
	 * spawn): 6 += (-0.005085, +0.05); 6 moves to (14.504588, 67.324883), its
	 * loop pushes 5: 5 += (+0.005395, -0.045973), 6 += (-0.005395, +0.045973).
	 * The port zeroes a squid's own motion at the end of its tick, so the
	 * dump's post-tick velocities ARE the impulses: 5 (0.010480, -0.095973),
	 * 6 (-0.005395, 0.045973). The scene is on the superflat (the push rule
	 * does not read the medium). */
	struct entity* q5 = mob_at(s, MONSTER_SQUID, 14.603788, 4.094616,
							   66.479485, 0.0F, 0.0F);
	struct entity* q6 = mob_at(s, MONSTER_SQUID, 14.5, 4.0, 67.5, 0.0F, 0.0F);
	check("S3 squid size", q5->width == 0.95F && q6->width == 0.95F,
		  "EntitySquid.setSize(0.95F, 0.95F)");
	check("S3 id order", q5->id < q6->id, "5 ticks before 6");
	struct push_expect w1 = {.pushed_vx = -0.005085, .pushed_vz = 0.05,
							 .mover_vx = 0.005085, .mover_vz = -0.05};
	scene_push("S3 5's loop pushes 6 at spawn", s, q5, q6, true, &w1);
	double var2, var4;
	assert(beta_apply_entity_collision(14.5, 67.5, 14.603788, 66.479485, &var2,
									   &var4));
	check("S3 first half", fabs(var4 + (double)0.05F) < 2e-8,
		  "abs_max 1.020515 = |dz| >= 1: Z half is the full -0.05F");

	/* 6 moves with that impulse consumed, then its loop finds 5 at 0.845398 in
	 * Z (< 1: the sqrt path, var8 = 1/0.919455 clamps to 1). Start 6 from the
	 * zeroed motion the port leaves after a squid's move. */
	q6->pos[0] = 14.504588F;
	q6->pos[2] = 67.324883F;
	q6->vel[0] = q6->vel[2] = 0.0F;
	test_mob_sync(q6);
	server_local_refresh_entity_membership(s, q6);
	struct push_expect w2 = {.pushed_vx = 0.010480, .pushed_vz = -0.095973,
							 .mover_vx = -0.005395, .mover_vz = 0.045973};
	scene_push("S3 6's loop pushes 5 after 6's move", s, q6, q5, true, &w2);
	remove_mob(s, q5);
	remove_mob(s, q6);
	section_done("S3 column_dedup_state: squid pack 5/6 at server tick 241 -- "
				 "both impulses land on 5 (repeated-call semantics)");
}


/* ---- S4: b0931e6e -- the swept clip lands exactly on the block face ------- */
/* Beta AxisAlignedBB.calculateX/YOffset [AxisAlignedBB.java:104-195] for one
 * block box whose fixed axes strictly overlap the mover: +d with old.max <=
 * box.min -> var3 = box.min - old.max (min over boxes); -d with old.min >=
 * box.max -> var3 = box.max - old.min (max over boxes). One box here. */
static double beta_clip_offset(double old_lo, double old_hi, double box_lo,
							   double box_hi, double d) {
	if(d > 0.0 && old_hi <= box_lo) {
		double var3 = box_lo - old_hi;
		return var3 < d ? var3 : d;
	}
	if(d < 0.0 && old_lo >= box_hi) {
		double var3 = box_hi - old_lo;
		return var3 > d ? var3 : d;
	}
	return d;
}

static void s4_exact_face_landing(struct server_local* s) {
	/* (a) The recorded sheep: split_off tick 20 CRPC_ENTITY_MOVE for id 1 at
	 * (6.398084, 4.004537, 44.490501) -> y 4.000000. A sheep body is 0.9 x 1.3
	 * with posY at its feet [EntitySheep.setSize(0.9F, 1.3F)]; the grass top
	 * of the superflat is the face at y = 4. The resting Y move is the port's
	 * -1.8 (the idiom every grounded mob carries; only the clip matters). */
	struct entity* sh = mob_at(s, MONSTER_SHEEP, 6.398084, 4.004537, 44.490501,
							   0.0F, 0.0F);
	struct AABB body = {-0.45F, 0.0F, -0.45F, 0.45F, 1.3F, 0.45F};
	vec3 pos = {sh->pos[0], sh->pos[1], sh->pos[2]};
	vec3 vel = {0.0F, -1.8F, 0.0F};
	bool collision_xz = false, on_ground = false;
	entity_try_move(sh, pos, vel, &body, 1, &collision_xz, &on_ground);
	double want = (double)4.004537F
		+ beta_clip_offset((double)4.004537F, (double)4.004537F + 1.3, 3.0,
						   4.0, -1.8);
	check("S4 sheep landing", on_ground && vel[1] == 0.0F,
		  "a downward move onto the floor grounds the mob and zeroes vel y");
	check("S4 sheep landing", fbits(pos[1]) == fbits(4.0F),
		  "feet must land EXACTLY on the face y = 4 (was 4.004537)");
	near_or_die("S4 sheep landing vs Beta calculateYOffset", pos[1], want, 0.0);
	remove_mob(s, sh);

	/* (b) The recorded item: column walk tick 60, item 1 (a 0.25 x 0.25 box,
	 * posY - 0.125 at its bottom [EntityItem.setSize(0.25F, 0.25F), yOffset =
	 * height / 2]) at x 88.871231 moving +X into a block face at x = 89 ->
	 * 88.875000 (= 89 - 0.125). Reproduced on the superflat inside the resident
	 * square: a stone cell at (9, 4, 18), the item at the same fraction. */
	struct entity* it = malloc(sizeof *it);
	assert(it);
	memset(it, 0, sizeof *it);
	entity_item(9901, it, true, &s->worlds[0],
				(struct item_data) {.id = BLOCK_COBBLESTONE, .count = 1});
	check("S4 item size", it->width == 0.25F && it->height == 0.25F
							  && it->y_offset == 0.125F,
		  "EntityItem.setSize(0.25F, 0.25F), yOffset 0.125F");
	assert(server_world_set_block(&s->worlds[0], 9, 4, 18,
								  (struct block_data) {.type = BLOCK_STONE}));
	it->teleport(it, (vec3) {8.871231F, 4.5F, 18.640610F});
	struct AABB ibox = {-0.125F, -0.125F, -0.125F, 0.125F, 0.125F, 0.125F};
	vec3 ipos = {it->pos[0], it->pos[1], it->pos[2]};
	vec3 ivel = {0.05F, 0.0F, 0.0F};
	collision_xz = false;
	on_ground = true;
	entity_try_move(it, ipos, ivel, &ibox, 0, &collision_xz, &on_ground);
	double iwant = (double)8.871231F
		+ beta_clip_offset((double)8.871231F - 0.125, (double)8.871231F + 0.125,
						   9.0, 10.0, 0.05);
	check("S4 item clip", collision_xz && ivel[0] == 0.0F,
		  "the +X move into the stone face is a horizontal collision");
	check("S4 item clip", fbits(ipos[0]) == fbits(8.875F),
		  "the box's +X face must land EXACTLY on x = 9 (centre 8.875)");
	near_or_die("S4 item clip vs Beta calculateXOffset", ipos[0], iwant, 1e-6);
	free(it);
	section_done("S4 b0931e6e swept clip: sheep lands on y = 4 exactly, item "
				 "stops on the x = 9 face exactly (AxisAlignedBB.calculateX/YOffset)");
}

/* ---- S5: d8b3595d -- the dropped item in a natural water current ---------- */
static bool beta_is_water(uint8_t t) {
	return t == BLOCK_WATER_FLOW || t == BLOCK_WATER_STILL;
}

static void cell(struct server_local* s, int x, int y, int z,
				 struct block_data* b) {
	if(!server_world_get_block(&s->worlds[0], x, y, z, b)) {
		b->type = BLOCK_AIR;
		b->metadata = 0;
	}
}

/* BlockFluid.getEffectiveFlowDecay [BlockFluid.java:35-45]: -1 for a
 * non-water cell, else metadata with >= 8 (falling) read as 0. */
static int beta_effective_decay(struct server_local* s, int x, int y, int z) {
	struct block_data b;
	cell(s, x, y, z, &b);
	if(!beta_is_water(b.type))
		return -1;
	return b.metadata >= 8 ? 0 : b.metadata;
}

/* Vec3D.normalize [Vec3D.java:63-66]: lengthVector = (float)sqrt(...);
 * < 1.0E-4D -> the zero vector. */
static void beta_normalize(double v[3]) {
	double len = (double)(float)sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if(len < 1.0E-4) {
		v[0] = v[1] = v[2] = 0.0;
		return;
	}
	v[0] /= len;
	v[1] /= len;
	v[2] /= len;
}

/* BlockFluid.getIsBlockSolid [BlockFluid.java:60-63]: water/ice never; else
 * Block.getIsBlockSolid = blockMaterial.isSolid. */
static bool beta_side_solid(struct server_local* s, int x, int y, int z) {
	struct block_data b;
	cell(s, x, y, z, &b);
	if(beta_is_water(b.type) || b.type == BLOCK_ICE)
		return false;
	return block_beta_material_is_solid(b.type);
}

/* BlockFluid.getFlowVector [BlockFluid.java:86-166]. */
static void beta_flow_vector(struct server_local* s, int x, int y, int z,
							 double out[3]) {
	static const int dx[4] = {-1, 0, 1, 0}, dz[4] = {0, -1, 0, 1};
	double v[3] = {0.0, 0.0, 0.0};
	int decay = beta_effective_decay(s, x, y, z);
	for(int i = 0; i < 4; i++) {
		int nx = x + dx[i], nz = z + dz[i];
		int nd = beta_effective_decay(s, nx, y, nz);
		if(nd < 0) {
			struct block_data nb;
			cell(s, nx, y, nz, &nb);
			if(!block_beta_material_is_solid(nb.type)) {
				nd = beta_effective_decay(s, nx, y - 1, nz);
				if(nd >= 0) {
					int k = nd - (decay - 8);
					v[0] += (double)((nx - x) * k);
					v[2] += (double)((nz - z) * k);
				}
			}
		} else {
			int k = nd - decay;
			v[0] += (double)((nx - x) * k);
			v[2] += (double)((nz - z) * k);
		}
	}
	struct block_data here;
	cell(s, x, y, z, &here);
	if(here.metadata >= 8
	   && (beta_side_solid(s, x, y, z - 1) || beta_side_solid(s, x, y, z + 1)
		   || beta_side_solid(s, x - 1, y, z) || beta_side_solid(s, x + 1, y, z)
		   || beta_side_solid(s, x, y + 1, z - 1)
		   || beta_side_solid(s, x, y + 1, z + 1)
		   || beta_side_solid(s, x - 1, y + 1, z)
		   || beta_side_solid(s, x + 1, y + 1, z))) {
		beta_normalize(v);
		v[1] += -6.0;
	}
	beta_normalize(v);
	out[0] = v[0];
	out[1] = v[1];
	out[2] = v[2];
}

/* World.handleMaterialAcceleration [World.java:1418-1450] over the box; the
 * impulse it adds (0.014D * normalized sum) in `imp`. Returns the inWater
 * verdict. checkChunksExist is the fixture's job (every chunk resident). */
static bool beta_material_acceleration(struct server_local* s, double min_x,
									   double min_y, double min_z,
									   double max_x, double max_y,
									   double max_z, double imp[3]) {
	int x0 = (int)floor(min_x), x1 = (int)floor(max_x + 1.0);
	int y0 = (int)floor(min_y), y1 = (int)floor(max_y + 1.0);
	int z0 = (int)floor(min_z), z1 = (int)floor(max_z + 1.0);
	bool hit = false;
	double v[3] = {0.0, 0.0, 0.0};
	for(int x = x0; x < x1; x++)
		for(int y = y0; y < y1; y++)
			for(int z = z0; z < z1; z++) {
				struct block_data b;
				cell(s, x, y, z, &b);
				if(!beta_is_water(b.type))
					continue;
				int m = b.metadata >= 8 ? 0 : b.metadata;
				double surface
					= (double)((float)(y + 1) - (float)(m + 1) / 9.0F);
				if((double)y1 >= surface) {
					hit = true;
					double f[3];
					beta_flow_vector(s, x, y, z, f);
					v[0] += f[0];
					v[1] += f[1];
					v[2] += f[2];
				}
			}
	imp[0] = imp[1] = imp[2] = 0.0;
	double len = (double)(float)sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if(len > 0.0) {
		beta_normalize(v);
		imp[0] = v[0] * 0.014;
		imp[1] = v[1] * 0.014;
		imp[2] = v[2] * 0.014;
	}
	return hit;
}

static void s5_item_water_current(struct server_local* s) {
	/* Walk tick 0 -> 1 rows for item 1 (seed 12345): pos (88.371841,
	 * 46.545109, 19.480774), vel (0.036766, 0.200000, -0.086584) -> after the
	 * tick (push, gravity, move, 0.98 drag) vel (0.045732, 0.156800,
	 * -0.094553) where the base gave (0.036030, 0.156800, -0.084852): the
	 * horizontal push is 0.014 along the unit (+1, 0, -1) current, and
	 * (0.036766 + 0.014/sqrt2) * 0.98 = 0.045732. */
	/* The recorded current, rebuilt on the superflat from BlockFluid's own
	 * decay rule: the item's cell (20, 4, 20) is flowing water decay 1, its +X
	 * neighbour decay 2 (k = +1 -> +X) and its -Z neighbour decay 2 (k = +1 ->
	 * -Z); the -X / +Z sides are stone (getFlowVector skips a solid). Sum
	 * (1, 0, -1), normalized -> the (+X, -Z) unit current the walk recorded. */
	struct block_data flow1 = {.type = BLOCK_WATER_FLOW, .metadata = 1};
	struct block_data flow2 = {.type = BLOCK_WATER_FLOW, .metadata = 2};
	struct block_data stone = {.type = BLOCK_STONE};
	assert(server_world_set_block(&s->worlds[0], 20, 4, 20, flow1));
	assert(server_world_set_block(&s->worlds[0], 21, 4, 20, flow2));
	assert(server_world_set_block(&s->worlds[0], 20, 4, 19, flow2));
	assert(server_world_set_block(&s->worlds[0], 19, 4, 20, stone));
	assert(server_world_set_block(&s->worlds[0], 20, 4, 21, stone));
	struct entity* it = malloc(sizeof *it);
	assert(it);
	memset(it, 0, sizeof *it);
	entity_item(9902, it, true, &s->worlds[0],
				(struct item_data) {.id = BLOCK_COBBLESTONE, .count = 1});
	it->teleport(it, (vec3) {20.5F, 4.5F, 20.5F});
	it->vel[0] = 0.036766F;
	it->vel[1] = 0.2F;
	it->vel[2] = -0.086584F;
	const float vx0 = it->vel[0], vy0 = it->vel[1], vz0 = it->vel[2];

	/* The PLAIN Entity.setPosition box: posX +- 0.125F, minY = posY - yOffset
	 * 0.125F + ySize 0, + 0.25F [EntityItem.handleWaterMovement:79-81]. */
	const double half = (double)(it->width / 2.0F);
	const double min_y
		= (double)it->pos[1] - (double)it->y_offset + (double)it->y_size;
	const double bx0 = (double)it->pos[0] - half, bx1 = (double)it->pos[0] + half;
	const double bz0 = (double)it->pos[2] - half, bz1 = (double)it->pos[2] + half;
	const double by1 = min_y + (double)it->height;

	double imp[3];
	bool beta_wet = beta_material_acceleration(s, bx0, min_y, bz0, bx1, by1,
											   bz1, imp);
	check("S5 the item sits in water", beta_wet, "the recorded cell is water");
	/* Vec3D.lengthVector is a FLOAT sqrt, so the unit components carry ~1e-8. */
	near_or_die("S5 current direction x", imp[0] / 0.014, 0.7071067811865476,
				1e-7);
	near_or_die("S5 current direction y", imp[1], 0.0, 0.0);
	near_or_die("S5 current direction z", imp[2] / 0.014, -0.7071067811865476,
				1e-7);

	bool wet = entity_water_acceleration(it, bx0, min_y, bz0, bx1, by1, bz1);
	check("S5 production verdict", wet == beta_wet, "inWater verdict");
	bits_or_die("S5 vx after push", it->vel[0], (double)vx0 + imp[0]);
	bits_or_die("S5 vy after push", it->vel[1], (double)vy0 + imp[1]);
	bits_or_die("S5 vz after push", it->vel[2], (double)vz0 + imp[2]);
	/* The recorded post-tick values, undone of EntityItem's 0.98 drag
	 * [EntityItem.java:69]: 0.045732 / 0.98, -0.094553 / 0.98. */
	near_or_die("S5 recorded vx (pre-drag)", it->vel[0], 0.045732 / 0.98, 2e-6);
	near_or_die("S5 recorded vz (pre-drag)", it->vel[2], -0.094553 / 0.98, 2e-6);
	free(it);
	section_done("S5 d8b3595d water current: item 1 of the seed-12345 walk "
				 "takes 0.014D along the (+X, -Z) flow (handleMaterialAcceleration"
				 " + getFlowVector)");
}

/* S6 -- the column_dedup_state tick-568 link (d8b3595d). Per-consumer rand_src
 * draw log (scratch, GOLDEN-result.md): base and base+d8b3595d draw the same
 * seeds through server tick 568 (current_tick 568 -> 569, walk tick 564);
 * inside that tick skeleton 17 draws ONE extra value -- the mob water-branch
 * swim roll (entity_monster.c `rand_gen_flt(&s->rand_src) <
 * MOB_WATER_JUMP_CHANCE`) -- because its inWater verdict flipped, and every
 * later server_world_random_tick pick of the same tick shifts by one draw, so
 * the sweep random-ticks flowing lava (87,10,50) where the base picked
 * (-100,3,153). The flip is Beta's own verdict: Entity.handleWaterMovement
 * [Entity.java:543-545] = handleMaterialAcceleration [World.java:1418-1450] over
 * the setPosition box expand(0,-0.4F,0) [AxisAlignedBB.java:90-98] contracted
 * 0.001 [func_28195_e :210-218]. At tick 569 the box's max X (46.106343)
 * reaches column 46, where (46,47,41) is water meta 1: surface 48 - 2/9F =
 * 47.777..., floor(maxY + 1) = 49 >= it -> wet. The foot cell d8b3595d
 * retired, (45,47,41), is air (the base's "dry"). At tick 568 (x 45.689999) the
 * box ends at 45.988999 and scans column 45 only -> dry on both definitions.
 * (Beta draws the swim roll from the ENTITY's Random [Entity.java:50], not the
 * world's; the port's single rand_src is what couples it to the random-tick
 * sweep -- a pre-existing port convention, docs/briefs/00_engine_contracts.md
 * section 5, not something d8b3595d introduced.) */
static bool beta_handle_water_movement(struct server_local* s, double px,
									   double min_y, double pz, float width,
									   float height, double imp[3]) {
	const double half = (double)(width / 2.0F);
	double x0 = px - half, x1 = px + half;
	double y0 = min_y, y1 = min_y + (double)height;
	double z0 = pz - half, z1 = pz + half;
	y0 -= (double)-0.4F; /* expand(0, -0.4F, 0): minY - y */
	y1 += (double)-0.4F; /*                      maxY + y */
	x0 += 0.001; y0 += 0.001; z0 += 0.001; /* func_28195_e(0.001 x3) */
	x1 -= 0.001; y1 -= 0.001; z1 -= 0.001;
	return beta_material_acceleration(s, x0, y0, z0, x1, y1, z1, imp);
}

static void s6_skeleton_box_water(struct server_local* s) {
	/* The recorded neighbourhood of skeleton 17 (seed 12345, server tick 569,
	 * scratch C-dump), cell for cell; every other cell of the scan is air. */
	static const struct { int x, y, z, type, meta; } scene[] = {
		{44, 46, 40, BLOCK_STONE, 0}, {44, 46, 41, BLOCK_STONE, 0},
		{44, 46, 42, BLOCK_STONE, 0}, {45, 46, 40, BLOCK_STONE, 0},
		{45, 46, 41, BLOCK_STONE, 0}, {45, 46, 42, BLOCK_STONE, 0},
		{46, 46, 40, BLOCK_WATER_STILL, 10}, {46, 46, 41, BLOCK_STONE, 0},
		{46, 46, 42, BLOCK_STONE, 0}, {46, 47, 40, BLOCK_WATER_STILL, 2},
		{46, 47, 41, BLOCK_WATER_STILL, 1}, {46, 47, 42, BLOCK_WATER_STILL, 11},
		{46, 48, 42, BLOCK_WATER_STILL, 3},
	};
	for(size_t i = 0; i < sizeof scene / sizeof scene[0]; i++) {
		struct block_data b = {.type = (uint8_t)scene[i].type,
							   .metadata = (uint8_t)scene[i].meta};
		/* The fixture runs no client: keep the bounded client inbox from
		 * filling (the live world publishes every set_block to slot 0). */
		clin_drain_discard();
		assert(server_world_set_block(&s->worlds[0], scene[i].x, scene[i].y,
									  scene[i].z, b));
	}
	struct block_data foot;
	cell(s, 45, 47, 41, &foot);
	check("S6 retired foot cell (45,47,41) is air", foot.type == BLOCK_AIR,
		  "the base's foot-cell probe read dry here");

	struct entity* sk = mob_at(s, MONSTER_SKELETON, 45.807343, 47.003555,
							   41.341072, 0.057809F, -0.087047F);
	check("S6 skeleton body", sk->width == 0.6F && sk->height == 1.8F
		  && sk->y_offset == 0.0F && sk->y_size == 0.0F, "0.6 x 1.8, no offset");

	/* Tick 569: box reaches column 46 -> wet; production == Beta, bit-exact
	 * impulse. */
	double imp[3];
	const double min_y
		= (double)sk->pos[1] - (double)sk->y_offset + (double)sk->y_size;
	bool beta_wet = beta_handle_water_movement(s, (double)sk->pos[0], min_y,
											   (double)sk->pos[2], sk->width,
											   sk->height, imp);
	check("S6 tick 569 Beta verdict", beta_wet,
		  "box max X 46.106343 scans column 46: (46,47,41) water meta 1");
	const float vx0 = sk->vel[0], vy0 = sk->vel[1], vz0 = sk->vel[2];
	bool wet = entity_handle_water_movement(sk, (double)sk->pos[0], min_y,
											(double)sk->pos[2], sk->width,
											sk->height);
	check("S6 tick 569 production verdict", wet == beta_wet, "inWater verdict");
	bits_or_die("S6 vx after push", sk->vel[0], (double)vx0 + imp[0]);
	bits_or_die("S6 vy after push", sk->vel[1], (double)vy0 + imp[1]);
	bits_or_die("S6 vz after push", sk->vel[2], (double)vz0 + imp[2]);

	/* Tick 568: x 45.689999 -> box max X 45.988999, column 45 only -> dry,
	 * no impulse. */
	sk->pos[0] = 45.689999F;
	sk->pos[2] = 41.477566F;
	sk->vel[0] = 0.067448F;
	sk->vel[2] = -0.071483F;
	test_mob_sync(sk);
	bool beta_wet_568 = beta_handle_water_movement(
		s, (double)sk->pos[0], min_y, (double)sk->pos[2], sk->width,
		sk->height, imp);
	check("S6 tick 568 Beta verdict", !beta_wet_568, "column 45 only: dry");
	const float ux0 = sk->vel[0], uy0 = sk->vel[1], uz0 = sk->vel[2];
	bool wet_568 = entity_handle_water_movement(sk, (double)sk->pos[0], min_y,
												(double)sk->pos[2], sk->width,
												sk->height);
	check("S6 tick 568 production verdict", !wet_568, "dry");
	check("S6 tick 568 no impulse",
		  fbits(sk->vel[0]) == fbits(ux0) && fbits(sk->vel[1]) == fbits(uy0)
			  && fbits(sk->vel[2]) == fbits(uz0),
		  "velocity untouched");
	remove_mob(s, sk);
	section_done("S6 d8b3595d column_dedup tick 568/569: skeleton 17's Beta box "
				 "reaches water at (46,47,41) while its foot cell is air -- the "
				 "inWater flip behind the one extra rand_src draw");
}

int main(void) {
	assert(clin_init());
	blocks_init();
	items_init();
	struct server_local* s = fixture();

	s1_split_off_chickens(s);
	s2_spawn_pack(s);
	s3_squid_pack(s);
	s4_exact_face_landing(s);
	s5_item_water_current(s);
	s6_skeleton_box_water(s);
	fixture_destroy(s);
	if(s_failures) {
		fprintf(stderr, "test_golden_move_divergence: %d of %d checks FAILED\n",
				s_failures, s_checks);
		return 1;
	}
	printf("PASS test_golden_move_divergence (%d checks)\n", s_checks);
	return 0;
}
