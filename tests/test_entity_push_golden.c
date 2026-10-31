/*
	test_entity_push_golden -- next-gaps-0924/W2 MOVE card 2 (P4). FAITHFULNESS
	golden (Beta 1.7.3, mcb173-retromcp src_original) for the entity-entity
	push, authored BEFORE the port. Intent: docs/intent/p4-entity-push.md.

	Spec:
	  Entity.applyEntityCollision:668-691 measures horizontal mover separation.
	      Use the larger absolute component [MathHelper.java:36-46] and require
	      at least 0.01F promoted to double. Take its float-rounded square root
	      [sqrt_double:18-20], rather than the Euclidean distance.
	      Divide the components by this divisor and scale by its reciprocal,
	      clamped to at most one, then by 0.05F and one minus the collision reduction.
	      The reduction defaults to zero (Entity.java:49, never set in 1.7.3).
	      Apply opposite impulses immediately; exclude both rider relationships (:669).
	  EntityLiving.onLivingUpdate:641-651 queries neighbours after heading movement
	      in the box expanded horizontally by 0.2F, without vertical expansion.
	      Each pushable neighbour receives the collision call.
	  World.getEntitiesWithinAABBExcludingEntity:2007-2023 +
	  Chunk.getEntitiesWithinAABBForEntity:484-506   chunk X asc, chunk Z asc,
	      section asc, insertion order; strict intersectsWith (AxisAlignedBB:185).
	  EntityLiving.canBePushed:85-87 requires a nondead body; Entity.canBePushed:714-716 rejects
	      (items never move; carts/boats have their own overrides -- follow-up).
	  Mover order in a tick = World.updateEntities' loadedEntityList order =
	      this port's id-ascending tick keys (spawn order here); impulses land
	      on the neighbour's motion at once (repeated-call semantics).

	HOW THE NUMBERS WERE MADE (CODESTYLE 4.2): tmp/jobs/next-gaps-0924/
	W2M-p4-golden.py -- the Beta behavior above, float32 storage for pos/vel as this
	port stores them, double math, the grounded friction from three float multiplications of 546.0F by 0.1F
	(EntityLiving.moveEntityWithHeading:491-497, bit-equal to float slipperiness 0.6F times friction 0.91F).
	Output: W2M-p4-golden.out. Nothing here ran CaveWii. Impulse values are
	compared BIT-EXACT (pure double + one float sqrt); multi-tick sequences
	within VEL_TOL / POS_TOL (the engine's float move/friction rounding).
	Raw-Beta traceability (CODESTYLE 2.4): 0.05F per unit of clamped
	direction; the float-rounded square root of a 0.4 gap, scaled by 0.05, gives 0.0316227773.

	Sections: 1 impulse values; 2 two mobs (A then B; B is pushed by A's loop
	BEFORE its own move -- repeated-call); 3 three mobs in a row (B is pushed
	both ways, list order A, C); 4 the 0.8 strict gate; 5 an item is never
	pushed; 6 the server player loop pushes the mob only (player half
	dropped); a mob's own loop with the player as neighbour pushes itself;
	7 the client player's own loop pushes the player only.
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
#include "network/server_interface.h" /* MAX_PLAYER_HEALTH */
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/input_owner.h"
#include "platform/thread.h"
#include "world.h"

#define VEL_TOL 1e-6
#define POS_TOL 2e-5

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

static uint32_t fbits(float f) {
	uint32_t u;
	memcpy(&u, &f, sizeof u);
	return u;
}

static void bits_or_die(const char* what, float got, double want) {
	s_checks++;
	if(fbits(got) != fbits((float)want)) {
		char msg[160];
		snprintf(msg, sizeof(msg), "got %.10g (%08x) want %.10g (%08x)", got,
				 fbits(got), (float)want, fbits((float)want));
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

static void sset(int x, int y, int z, int type) {
	struct block_data cur;
	if(server_world_get_block(&s.worlds[0], x, y, z, &cur) && cur.type == type
	   && cur.metadata == 0)
		return;
	assert(server_world_set_block(&s.worlds[0], x, y, z,
								  (struct block_data) {.type = (uint8_t)type}));
}

static void platform(int x0, int x1, int z0, int z1) {
	for(int x = x0; x <= x1; x++)
		for(int z = z0; z <= z1; z++)
			sset(x, 100, z, BLOCK_STONE);
}

/* W1B settle idiom (floor + 2): rest by the production tick, feet at 101. */
static struct entity* spawn_settled(float x, float z) {
	vec3 pos = {x, 102.0F, z};
	struct entity* m = server_local_spawn_monster(pos, MONSTER_PIG_ZOMBIE, &s);
	assert(m && m->tick_server);
	for(int i = 0; i < 120; i++) {
		m->vel[0] = m->vel[2] = 0.0f;
		m->pos[0] = x;
		m->pos[2] = z;
		test_mob_sync(m); /* motion units S2 */
		m->data.monster.path.length = 0;
		m->data.monster.knockback_time = 100;
		bool reaped = m->tick_server(m, &s);
		assert(!reaped && "mob must survive the settle");
	}
	assert(floorf(m->pos[1]) == 101.0F && m->pos[1] < 101.05F);
	m->data.monster.path.length = 0;
	m->data.monster.direction[0] = m->data.monster.direction[1] = 0.0f;
	/* W1B's idiom: X/Z only. vel[1] carries the engine's resting -1.8 that the
	 * next Y-move consumes to re-assert onGround; zeroing it makes that move
	 * zero-length, prev_on_ground reads false for one tick and Beta's friction
	 * correctly picks the airborne 0.91F -- a fixture artefact, not physics. */
	m->vel[0] = m->vel[2] = 0.0f;
	test_mob_sync(m); /* motion units S2 */
	return m;
}

static void mob_tick(struct entity* m) {
	m->data.monster.knockback_time = 100; /* no walk push, no Wii cap */
	m->data.monster.path.length = 0;
	bool reaped = m->tick_server(m, &s);
	assert(!reaped);
}

struct xv {
	double x, vx;
};

static void test_impulse_values(void) {
	double dx, dz;
	check("1 gate", !entity_collision_impulse(0.0, 0.0, 0.005, 0.0, &dx, &dz),
		  "0.005 < 0.01F must not push");
	struct {
		double mx, mz, want_x, want_z;
	} k[] = {
		{0.4, 0.0, 0.03162277728546167, 0.0},
		{2.0, 0.0, 0.050000002456485226, 0.0},
		{0.3, 0.4, 0.023717082964096255, 0.03162277728546167},
		{-0.4, 0.0, -0.03162277728546167, 0.0},
		{0.1, 0.1, 0.015811388642730836, 0.015811388642730836},
		{1.0, 0.0, 0.05000000074505806, 0.0},
	};
	for(unsigned i = 0; i < sizeof k / sizeof k[0]; i++) {
		bool pushed = entity_collision_impulse(0.0, 0.0, k[i].mx, k[i].mz, &dx, &dz);
		s_checks++;
		if(!pushed || dx != k[i].want_x || dz != k[i].want_z) {
			char msg[200];
			snprintf(msg, sizeof msg,
					 "mover (%g,%g): got (%.17g,%.17g) want (%.17g,%.17g)",
					 k[i].mx, k[i].mz, dx, dz, k[i].want_x, k[i].want_z);
			fail("1 impulse", msg);
		}
	}
	section_done("1 applyEntityCollision impulses bit-exact (abs_max, float sqrt, 1/d clamp, 0.05F)");
}

static void test_two_mobs(void) {
	static const struct xv ka[] = {{5.500000000, -0.064471789},
								   {5.435528278, -0.108544268},
								   {5.326983929, -0.100797422}};
	static const struct xv kb[] = {{5.931622982, 0.050115049},
								   {6.016954899, 0.084716968},
								   {6.143204212, 0.068932079}};
	struct entity* a = spawn_settled(5.5F, 8.5F);
	struct entity* b = spawn_settled(9.5F, 8.5F);
	assert(a->id < b->id);
	b->pos[0] = 5.9F;
	test_mob_sync(b); /* motion units S2 */
	for(int t = 0; t < 3; t++) {
		mob_tick(a);
		mob_tick(b);
		near_or_die("2 A x", t, a->pos[0], ka[t].x, POS_TOL);
		near_or_die("2 A vx", t, a->vel[0], ka[t].vx, VEL_TOL);
		near_or_die("2 B x", t, b->pos[0], kb[t].x, POS_TOL);
		near_or_die("2 B vx", t, b->vel[0], kb[t].vx, VEL_TOL);
		near_or_die("2 z untouched", t, a->pos[2], 8.5, 0.0);
		near_or_die("2 z untouched", t, b->pos[2], 8.5, 0.0);
	}
	section_done("2 two overlapping mobs part (A's loop pushes B before B's own move)");
}

static void test_three_mobs(void) {
	static const struct xv ka[] = {{5.500000000, -0.064471789},
								   {5.435528278, -0.106457025},
								   {5.329071045, -0.137996450}};
	static const struct xv kb[] = {{5.931622982, -0.011804298},
								   {5.955035686, -0.015502458},
								   {5.979092121, -0.016067453}};
	static const struct xv kc[] = {{6.330347061, 0.048141789},
								   {6.409120083, 0.076703019},
								   {6.518611431, 0.096508235}};
	struct entity* a = spawn_settled(5.5F, 12.5F);
	struct entity* b = spawn_settled(9.5F, 12.5F);
	struct entity* c = spawn_settled(13.5F, 12.5F);
	assert(a->id < b->id && b->id < c->id);
	b->pos[0] = 5.9F;
	test_mob_sync(b); /* motion units S2 */
	c->pos[0] = 6.3F;
	test_mob_sync(c); /* motion units S2 */
	for(int t = 0; t < 3; t++) {
		mob_tick(a);
		mob_tick(b);
		mob_tick(c);
		near_or_die("3 A x", t, a->pos[0], ka[t].x, POS_TOL);
		near_or_die("3 A vx", t, a->vel[0], ka[t].vx, VEL_TOL);
		near_or_die("3 B x", t, b->pos[0], kb[t].x, POS_TOL);
		near_or_die("3 B vx", t, b->vel[0], kb[t].vx, VEL_TOL);
		near_or_die("3 C x", t, c->pos[0], kc[t].x, POS_TOL);
		near_or_die("3 C vx", t, c->vel[0], kc[t].vx, VEL_TOL);
	}
	section_done("3 three mobs in a row: middle one pushed both ways in list order");
}

static void test_gate(void) {
	struct entity* a = spawn_settled(5.5F, 16.5F);
	struct entity* b = spawn_settled(9.5F, 16.5F);
	b->pos[0] = 6.29F; /* gap 0.79: B starts at 5.99, inside A's expanded limit 6.0; strict hit */
	test_mob_sync(b); /* motion units S2 */
	mob_tick(a);
	mob_tick(b);
	near_or_die("4 dx 0.79 A vx", 0, a->vel[0], -0.044440974, VEL_TOL);
	near_or_die("4 dx 0.79 B vx", 0, b->vel[0], 0.024264775, VEL_TOL);
	struct entity* c = spawn_settled(5.5F, 20.5F);
	struct entity* d = spawn_settled(9.5F, 20.5F);
	d->pos[0] = 6.3F; /* gap 0.8000002: D starts beyond the expanded limit 6.0; no hit */
	mob_tick(c);
	mob_tick(d);
	near_or_die("4 dx 0.80 C vx", 0, c->vel[0], 0.0, 0.0);
	near_or_die("4 dx 0.80 D vx", 0, d->vel[0], 0.0, 0.0);
	section_done("4 strict intersectsWith: 0.79 pushes, 0.80 does not");
}

static void test_item_filter(void) {
	struct entity* a = spawn_settled(5.5F, 24.5F);
	struct entity* it = malloc(sizeof *it);
	assert(it);
	memset(it, 0, sizeof *it);
	entity_item(901, it, true, &s.worlds[0],
				(struct item_data) {.id = BLOCK_COBBLESTONE, .count = 1});
	it->teleport(it, (vec3) {5.9F, 101.125F, 24.5F});
	glm_vec3_zero(it->vel);
	it->data.item.was_in_water = true;
	*dict_entity_safe_get(s.entities, it->id) = it; /* a world entity */
	server_local_refresh_entity_membership(&s, it);
	mob_tick(a);
	near_or_die("5 mob vx", 0, a->vel[0], 0.0, 0.0);
	near_or_die("5 item vx", 0, it->vel[0], 0.0, 0.0);
	assert(!it->tick_server(it, &s));
	near_or_die("5 item vx after its own tick", 0, it->vel[0], 0.0, 0.0);
	dict_entity_erase(s.entities, it->id);
	free(it);
	section_done("5 Entity.canBePushed false: a dropped item is never pushed and pushes nothing");
}

/* Entity.applyEntityCollision (S/Entity.java:630-655) transcribed for the
 * check-6 re-derivation: the receiver is the pushed neighbour; the mover owns the
 * running sweep; returns the mover's positive half, with its opposite on the receiver. */
static bool beta_apply_collision(double this_x, double this_z, double mover_x,
								 double mover_z, double* hx, double* hz) {
	double delta_x = mover_x - this_x, delta_z = mover_z - this_z;
	double a2 = delta_x < 0.0 ? -delta_x : delta_x, a4 = delta_z < 0.0 ? -delta_z : delta_z;
	double separation_scale = a2 > a4 ? a2 : a4;
	if(!(separation_scale >= (double)0.01F))
		return false;
	separation_scale = (double)(float)sqrt(separation_scale);
	delta_x /= separation_scale;
	delta_z /= separation_scale;
	double reciprocal_clamp = 1.0 / separation_scale;
	if(reciprocal_clamp > 1.0)
		reciprocal_clamp = 1.0;
	*hx = delta_x * reciprocal_clamp * (double)0.05F * (double)(1.0F - 0.0F);
	*hz = delta_z * reciprocal_clamp * (double)0.05F * (double)(1.0F - 0.0F);
	return true;
}

/* Check 6, RE-DERIVED for the repaired schedule (G8-BLAST-VELOCITY v4,
 * ASTRA-V3-REVIEW P1 push): Beta runs the server player's own loop ONLY
 * inside NetServerHandler.handleFlying -> onUpdateEntity -> EntityLiving.
 * onLivingUpdate:623-629, once per accepted movement packet, at the box
 * moveEntityWithHeading left -- and the player's half is REAL: its
 * EntityPlayerMP.motionX/Z (Entity.addVelocity, S/Entity.java:651), which
 * the tracker sends with the next fresh hit. So:
 *   6a zero packets: the per-tick hook pushes NOTHING (live body);
 *   6b one packet at the player's box: the mob takes -half, the player's
 *      packet motion +half (bit-exact, independent transcription);
 *   6c the mob's own loop with the player as neighbour (world phase):
 *      the player's server motion takes that half too;
 *   6d out of reach: nothing.
 * (Before v4: "player half dropped"; v3 delivered it once per UPDATE at
 * the current pose before the replay -- both wrong schedules.) */
static void test_server_player_loop(void) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.x = 5.5;
	s.player.y = 101.0 + 1.62;
	s.player.z = 28.5;
	s.player.vel_x = s.player.vel_z = 0.0;
	server_local_refresh_player_membership(&s);
	struct entity* d = spawn_settled(9.5F, 28.5F);
	d->pos[0] = 5.9F;
	test_mob_sync(d); /* motion units S2 */
	/* 6a: no packet this update -> no own loop for a LIVE body (v5: the
	 * network-phase replay with an empty queue). */
	server_local_test_replay_actor_motion(&s);
	near_or_die("6a zero packets: mob untouched", 0, d->vel[0], 0.0, 0.0);
	near_or_die("6a zero packets: player untouched", 0, s.player.vel_x, 0.0, 0.0);
	/* 6b: one accepted packet's loop at the player's box (width 0.6F,
	 * height 1.8F, feet 1.62F below the eye). */
	const double half = (double)(0.6F / 2.0F);
	const double feet = s.player.y - (double)1.62F;
	double mx = s.player.vel_x, mz = s.player.vel_z;
	entity_actor_collide_with_nearby(&s, &s.worlds[0], &s.player,
		s.player.x - half, feet, s.player.z - half, s.player.x + half,
		feet + (double)1.8F, s.player.z + half, &mx, &mz);
	s.player.vel_x = mx; /* the replay stores the packet's motion */
	s.player.vel_z = mz;
	/* The mob sits at FLOAT 5.9 (5.900000095): dx = 0.4000000954, impulse
	 * 0.03162278184468655 (W2M-p4-golden-67.out), one ulp above the exact-0.4
	 * value of section 1. */
	bits_or_die("6b mob half from the player's packet loop", d->vel[0],
				0.03162278184468655);
	double hx, hz;
	assert(beta_apply_collision((double)5.9F, 28.5, 5.5, 28.5, &hx, &hz));
	check("6b player half on its server motion", s.player.vel_x == hx
		  && s.player.vel_z == hz && hx < 0.0, "player half != Beta +half");
	near_or_die("6b player half value", 0, s.player.vel_x, -0.03162278184468655,
				1e-17);
	near_or_die("6 z", 0, d->vel[2], 0.0, 0.0);
	/* 6c: the mob's own loop with the player as the neighbour: move by
	 * 0.0316, friction, then a push scaled from the square root of gap 0.4316 -- the two-mob B t0
	 * recurrence with a static A; the player takes that loop's -half. */
	const double player_before = s.player.vel_x;
	mob_tick(d);
	near_or_die("6c mob x after its loop", 0, d->pos[0], 5.931622982, POS_TOL);
	near_or_die("6c mob vx after its loop", 0, d->vel[0], 0.050115049, VEL_TOL);
	double mhx, mhz;
	assert(beta_apply_collision(5.5, 28.5, (double)d->pos[0], 28.5, &mhx, &mhz));
	check("6c player takes the mob loop's half", s.player.vel_x
		  == player_before - mhx, "player != before - mob-loop half");
	near_or_die("6c player total", 0, s.player.vel_x, -0.0644717914, 1e-9);
	/* 6d: out of reach: nothing. */
	s.player.x = 20.5;
	server_local_refresh_player_membership(&s);
	float before = d->vel[0];
	mx = s.player.vel_x;
	mz = s.player.vel_z;
	entity_actor_collide_with_nearby(&s, &s.worlds[0], &s.player,
		s.player.x - half, feet, s.player.z - half, s.player.x + half,
		feet + (double)1.8F, s.player.z + half, &mx, &mz);
	near_or_die("6d far player", 0, d->vel[0], before, 0.0);
	check("6d far player motion", mx == s.player.vel_x && mz == s.player.vel_z,
		  "far loop changed the player");
	s.player.vel_x = s.player.vel_z = 0.0;
	s.player.has_pos = false;
	section_done("6 server player loop (v4, per accepted packet): mob takes -half, player +half on its server motion; zero packets push nothing");
}

/* ---- P4b sections (Opus review of aa8bfa1c) ------------------------------ */

/* MED-1: Entity.java:669 -- the host player and the pig it rides push each
 * other in neither direction; unsaddling restores the push. */
/* v4: ONE accepted movement packet's own push loop for the host body at its
 * current box (what the server replay runs after the packet's physics); the
 * player's half is stored to its server motion, as the replay does. */
static void player_packet_loop(void) {
	const double half = (double)(0.6F / 2.0F);
	const double feet = s.player.y - (double)1.62F;
	double mx = s.player.vel_x, mz = s.player.vel_z;
	entity_actor_collide_with_nearby(&s, &s.worlds[0], &s.player,
		s.player.x - half, feet, s.player.z - half, s.player.x + half,
		feet + (double)1.8F, s.player.z + half, &mx, &mz);
	s.player.vel_x = mx;
	s.player.vel_z = mz;
}

static void test_mount_gate(void) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 5.5;
	s.player.y = 101.0 + 1.62;
	s.player.z = 30.5;
	server_local_refresh_player_membership(&s);
	vec3 pos = {5.5F, 102.0F, 30.5F};
	struct entity* pig = server_local_spawn_monster(pos, MONSTER_PIG, &s);
	assert(pig && pig->tick_server);
	pig->pos[0] = 5.9F;
	pig->pos[1] = 101.0F;
	test_mob_sync(pig); /* motion units S2 */
	glm_vec3_zero(pig->vel);
	pig->data.monster.pig_ridden = true;
	pig->data.monster.pig_rider_id = 0;
	pig->data.monster.pig_rider_actor = server_local_actor_ident(&s, &s.player);
	s.player.vel_x = s.player.vel_z = 0.0;
	player_packet_loop(); /* v4: the rider's per-packet loop */
	near_or_die("8 ridden pig not pushed by its rider", 0, pig->vel[0], 0.0, 0.0);
	near_or_die("8 rider not pushed by its ridden pig", 0, s.player.vel_x, 0.0, 0.0);
	/* the pig as mover: its loop skips its rider, so no mob half either */
	pig->data.monster.knockback_time = 100;
	pig->data.monster.path.length = 0;
	glm_vec3_zero(pig->vel);
	pig->pos[0] = 5.9F;
	test_mob_sync(pig); /* motion units S2 */
	entity_living_collide_with_nearby(pig, &s);
	near_or_die("8 ridden pig's own loop skips its rider", 0, pig->vel[0], 0.0, 0.0);
	pig->data.monster.pig_ridden = false;
	player_packet_loop();
	/* pig 0.9 wide at float 5.9 vs player at 5.5: dx 0.4000000954 */
	bits_or_die("8 unsaddled: pushed again", pig->vel[0], 0.03162278184468655);
	near_or_die("8 unsaddled: the player takes its half", 0, s.player.vel_x,
				-0.03162278184468655, 1e-17);
	s.player.vel_x = s.player.vel_z = 0.0;
	pig->delay_destroy = 0; /* retire */
	s.player.has_pos = false;
	section_done("8 mount gate: a ridden pig and its rider never push each other");
}

/* LOW: a mob in its deathTime flop still runs its push loop and is pushable
 * (EntityLiving.onUpdate:203-206 runs onLivingUpdate unconditionally;
 * canBePushed rejects a body marked dead, after death tick 20, :155-159). */
static void test_dying_mob_pushes(void) {
	struct entity* a = spawn_settled(5.5F, 34.5F);
	struct entity* b = spawn_settled(9.5F, 34.5F);
	b->pos[0] = 5.9F;
	test_mob_sync(b); /* motion units S2 */
	a->health = 0; /* enters the flop on its tick */
	a->data.monster.knockback_time = 100;
	assert(!a->tick_server(a, &s));
	near_or_die("9 flopping mob still alive for the world", 0,
				(double)(a->delay_destroy != 0), 1.0, 0.0);
	bits_or_die("9 flopping mob pushes its neighbour", b->vel[0], 0.03162278184468655);
	/* and is still pushable: B's own loop pushes A */
	float a_before = a->vel[0];
	mob_tick(b);
	near_or_die("9 flopping mob still pushable", 0, (double)(a->vel[0] != a_before), 1.0, 0.0);
	a->delay_destroy = 0;
	b->delay_destroy = 0;
	section_done("9 dying mob: pushes and is pushed through the death flop");
}

/* LOW: the dead host player runs its own loop through the 21st dead tick and
 * is a candidate for 20 (EntityLiving.java:155-159 increments the death timer
 * and removes after tick 20 in onEntityUpdate, before onLivingUpdate).
 * v5 (ASTRA-V4-REVIEW dead-client loop): both run inside the corpse's
 * PACKETS -- one stationary corpse packet per update in the network phase
 * (server_local_test_replay_actor_motion), after the world's mob pass; the
 * exact corpse motion/moved-box payloads are test_explosion_beta_golden K1. */
static void test_dead_player_window(void) {
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 5.5;
	s.player.y = 101.0 + 1.62;
	s.player.z = 38.5;
	s.player.vel_x = s.player.vel_z = 0.0;
	s.player.motion_y = 0.0;
	s.player.motion_q_n = 0;
	server_local_refresh_player_membership(&s);
	struct entity* d = spawn_settled(9.5F, 38.5F);
	d->pos[0] = 5.9F;
	test_mob_sync(d); /* motion units S2 */
	short saved = s.player.health;
	s.player.health = 0;
	s.player.death_ticks = 0;
	int pushed_own = 0, pushed_as_candidate = 0;
	for(int k = 1; k <= 23; k++) {
		/* the mob's own tick first (dedicated-server order: world pass, then
		 * the network phase): reads the previous death_ticks */
		glm_vec3_zero(d->vel);
		d->pos[0] = 5.9F;
		test_mob_sync(d); /* motion units S2 */
		entity_living_collide_with_nearby(d, &s);
		if(d->vel[0] != 0.0F)
			pushed_as_candidate++;
		glm_vec3_zero(d->vel);
		test_mob_sync(d);
		/* the corpse packet keeps its pose (the dead client sends nothing) */
		s.player.x = 5.5;
		server_local_test_replay_actor_motion(&s);
		if(d->vel[0] != 0.0F)
			pushed_own++;
	}
	near_or_die("10 own loop runs 21 corpse packets", 0, pushed_own, 21.0, 0.0);
	near_or_die("10 candidate for 21 mob ticks (previous value 0..20)", 0,
				pushed_as_candidate, 21.0, 0.0);
	near_or_die("10 deathTime stops with the packets", 0, s.player.death_ticks,
				21.0, 0.0);
	s.player.health = saved > 0 ? saved : 160;
	glm_vec3_zero(d->vel);
	test_mob_sync(d);
	server_local_test_replay_actor_motion(&s); /* alive, no packet */
	near_or_die("10 alive with no packet: nothing pushes", 0,
				(double)(d->vel[0] != 0.0F), 0.0, 0.0);
	/* the respawned body's first packet: deathTime back to 0, its loop
	 * pushes again (server_local_tick_actor_knockback = one accepted
	 * stationary packet for every present body) */
	server_local_tick_actor_knockback(&s);
	near_or_die("10 respawned: its packet loop pushes again", 0,
				(double)(d->vel[0] != 0.0F), 1.0, 0.0);
	near_or_die("10 death_ticks reset", 0, s.player.death_ticks, 0.0, 0.0);
	d->delay_destroy = 0;
	s.player.has_pos = false;
	section_done("10 dead player: 21 corpse packets (own loop), 20-tick candidacy, reset by the next live packet");
}

/* ----------------------------------------------------------- client half */

static struct client_session s_owner;

static void cfill(int x0, int x1, int y0, int y1, int z0, int z1, uint8_t type) {
	for(int x = x0; x <= x1; x++)
		for(int y = y0; y <= y1; y++)
			for(int z = z0; z <= z1; z++)
				world_set_block(&gstate.world, x, y, z,
								(struct block_data) {.type = type}, false);
}

static void test_client_player_loop(void) {
	cfill(2, 9, 100, 100, 2, 9, BLOCK_STONE);
	struct entity e;
	memset(&e, 0, sizeof e);
	entity_local_player(91, &e, &gstate.world);
	e.pos[0] = 5.5F;
	e.pos[1] = 101.0F + 1.62F;
	e.pos[2] = 5.5F;
	e.orient[1] = GLM_PI_2;
	e.on_ground = true;
	e.data.local_player.capture_input = true;
	memset(&s_owner, 0, sizeof s_owner);
	s_owner.local_player = &e;
	s_owner.oxygen = MAX_OXYGEN;
	/* W1A's settle_on_floor: 4 ticks so the standing state is Beta's
	 * (steady vertical motion -0.0784, grounded every tick) before the mob copy
	 * appears; a vy=0 start makes tick 1's Y-move zero-length and tick 2
	 * would read the airborne 0.91F -- fixture artefact, not physics. */
	{
		struct input_player_frame settle = {.connected = true};
		for(int t = 0; t < 4; t++)
			assert(!entity_local_player_walk_owned(&e, &s_owner, &settle));
		assert(e.on_ground);
		e.pos[0] = 5.5F;
		e.pos[2] = 5.5F;
		e.vel[0] = e.vel[2] = 0.0F;
	}

	static struct entity m; /* the client's copy of a mob */
	memset(&m, 0, sizeof m);
	entity_monster(950, &m, false, &gstate.world, MONSTER_PIG_ZOMBIE);
	m.pos[0] = 5.9F;
	m.pos[1] = 101.0F;
	m.pos[2] = 5.5F;
	m.delay_destroy = -1;
	*dict_entity_safe_get(gstate.entities, 950) = &m;

	struct input_player_frame frame = {.connected = true};
	assert(!entity_local_player_walk_owned(&e, &s_owner, &frame));
	/* mob copy at float 5.9: impulse -0.03162278184468655 (float 0xbd0186e4). */
	/* P4b: the player takes BOTH halves -- its own loop's receiving impulse
	 * [Entity.java:686] and the mob copy's loop's opposite impulse
	 * [:685, :643-648] -- two float adds of -0.03162278184468655:
	 * -0.06324556469917297 (W2M-p4b-golden.out). */
	bits_or_die("7 player: own-loop half + mob-loop half", e.vel[0], -0.06324556469917297);
	near_or_die("7 mob copy untouched", 0, m.vel[0], 0.0, 0.0);
	assert(!entity_local_player_walk_owned(&e, &s_owner, &frame));
	near_or_die("7 player x tick 2", 1, e.pos[0], 5.43675422668457, POS_TOL);
	near_or_die("7 player vx tick 2", 1, e.vel[0], -0.10259424895048141, VEL_TOL);
	/* mount gate, client side [Entity.java:669]: the pig this player rides
	 * pushes it in neither direction. */
	m.data.monster.id = MONSTER_PIG;
	m.data.monster.pig_ridden = true;
	m.data.monster.pig_rider_id = e.id;
	e.pos[0] = 5.5F;
	e.vel[0] = 0.0F;
	assert(!entity_local_player_walk_owned(&e, &s_owner, &frame));
	near_or_die("7 ridden pig: no client push", 2, e.vel[0], 0.0, 0.0);
	dict_entity_erase(gstate.entities, 950);
	section_done("7 client player loop: both player halves against the mob copy; ridden pig inert");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_entity_push_golden_tmp", s_fixture, sizeof s_fixture);
	assert(clin_init());
	blocks_init();
	items_init();

	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);

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
	s.player.x = 8.5; /* positionless (has_pos false) until section 6 */
	s.player.y = 140.0;
	s.player.z = 16.0;

	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));

	struct server_chunk* sc = NULL;
	for(w_coord_t cx = -3; cx <= 3; cx++)
		for(w_coord_t cz = -3; cz <= 4; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
	platform(0, 15, 4, 40);

	test_impulse_values();
	test_two_mobs();
	test_three_mobs();
	test_gate();
	test_item_filter();
	test_server_player_loop();
	test_mount_gate();
	test_dying_mob_pushes();
	test_dead_player_window();
	test_client_player_loop();

	g_drain_run = 0;
	thread_join(&drain);
	if(s_failures) {
		fprintf(stderr, "test_entity_push_golden: %d of %d checks FAILED\n",
				s_failures, s_checks);
		return 1;
	}
	printf("PASS test_entity_push_golden (%d checks)\n", s_checks);
	return 0;
}
