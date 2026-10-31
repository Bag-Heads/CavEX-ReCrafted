/*
	next-gaps-0924/W1C: dropped-item environment parity golden -- ground
	friction (P2), lava/fire burn and the landing bounce (P9).

	Spec = the Beta 1.7.3 decompile (mcb173-retromcp src_original):

	  EntityItem.java:8        health starts at 5
	  EntityItem.java:45       gravity takes (double)0.04F  (BEFORE moveEntity)
	  EntityItem.java:46-51    material at floor(pos) == lava -> motionY =
	                           (double)0.2F, motionX/Z = 0.2F times (first rand
	                           float minus second), "random.fizz" 0.4F,
	                           pitch 2.0F plus 0.4F of a rand float  (5 entity-rand draws)
	  EntityItem.java:55-66    friction 0.98F; grounded it is 0.1F*0.1F*58.8F,
	                           or over a non-air block at floor(bb.minY)-1 the
	                           slipperiness * 0.98F; it scales motionX/Z,
	                           and motionY is scaled by (double)0.98F
	  EntityItem.java:67-69    onGround -> motionY *= -0.5D
	  EntityItem.java:83-95    dealFireDamage -> attackEntityFrom: health drops by dmg;
	                           health <= 0 -> setEntityDead (no hurt-resistance)
	  Entity.java:52-53        fireResistance = 1; fire = 0
	  Entity.java:203-216      burning, not fire-immune: every 20th fire count ->
	                           1 damage from a null source; fire counts down
	  Entity.java:218-240      handleLavaMovement (World.isMaterialInBB over
	                           boundingBox.expand(-0.1F,-0.4F,-0.1F)) ->
	                           attackEntityFrom(null, 4); fire = 600
	  Entity.java:424-426      a blocked Y move zeroes motionY (so the -0.5D
	                           "bounce" at :67-69 multiplies 0 -- Beta items never
	                           rebound; pinned in section 3)
	  Entity.java:480-496      isBoundingBoxBurning(bb.contract(0.001)) (fire OR
	                           lava ids, World.java:1395-1416) -> dealFireDamage(1);
	                           not wet -> fire counts up (reaching 0 jumps to 300);
	                           not burning: fire <= 0 resets to -fireResistance
	  Block.java:143 / BlockIce.java:8   slipperiness 0.6F / 0.98F

	Expected values are computed from those lines with Java float/double
	semantics (tmp/jobs/next-gaps-0924/W1C-golden.py, a Java-float transcription), NOT
	snapshotted from CaveWii:
	  ice   friction 0.98F*0.98F = 0x3f75dcc7; stone 0.6F*0.98F = 0x3f16872c
	  (0.1F*0.1F*58.8F rounds to the same float as stone -- no observable split)
	  ice slide  x0 3.5, v0 0.1F: t1 x 3.600000001 v 0.0960400059,
	             t10 x 4.339373985 v 0.0667608294, t30 x 5.273856684 v 0.0297553565
	  fall  y0 110.125 vy0 0.2F: t1 110.285000004, t4 110.509238755,
	        t27 101.531706497, lands t28 on a y=100 floor with motionY -0.0
	  lava  motionY after the dead tick = (float)(0.2F * 0.98F) = 0x3e48b43a
	  death ticks: lava with isMaterialInBB hit = tick 1 (5-4-1); lava whose
	  expanded box first reaches lava on tick 2 = tick 2 (5-1, then -4);
	  fresh item in fire = tick 5 (fire 0->1 oscillates, one burn per tick);
	  item already at fire=-1 = tick 3 (fire -1->0->300, DoT + burn each tick).

	RNG (item-rng-1003): Entity.java:41 owns Random; :1220 pushes with one
	nextFloat()*0.2F+0.1F; EntityItem.java:67-69 draws lava X pair, then Z
	pair, then fizz. Gameplay uses each item's Java48 lifetime state; the world
	and shared entity streams stay untouched. Fizz remains on the separate
	sound stream (one draw): retained Beta fifth-draw sequence debt.
	Raw RNG/float expectations below come from the RUN installed JRE oracle
	item-rng-1003/oracle/stdout.json (receipt.json pins runtime/script/output).

	W1C-b additions (Fable review): 4-7 assert the owned entity stream; a lava or
	pushOut tick sends CRPC_ENTITY_PHYSICS_CORRECTION with the server state;
	6 = void kill (Entity.java:222-224, posY < -64 read before the move);
	7 = pushOutOfBlocks (Entity.java:1056-1129): nearest open face +Y, an
	all-0.5 tie resolves to -X, a sealed cube still draws the roll but moves
	nothing; 8 = the cached burn probe equals the dict-scan original.
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
#include "item/items.h"
#include "network/client_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"
#include "util.h"

static uint32_t fbits(float f) {
	uint32_t u;
	memcpy(&u, &f, sizeof u);
	return u;
}

static char s_fixture_TMP_WORLD[128];
#define TMP_WORLD s_fixture_TMP_WORLD

static struct server_local s;
bool entity_item_bounding_box_burning(struct server_local* s,
									  struct entity* e); // test export

static struct {
	int count;
	struct client_rpc last;
} g_corr;
static void observe_rpc(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_ENTITY_PHYSICS_CORRECTION) {
		g_corr.count++;
		g_corr.last = *call;
	}
}

static bool correction_matches(const struct entity* e) {
	const struct entity_physics_correction_rpc* p
		= &g_corr.last.payload.entity_physics_correction;
	if(p->entity_id != e->id || p->on_ground != e->on_ground)
		return false;
	for(int k = 0; k < 3; k++)
		if(fbits(p->pos[k]) != fbits(e->pos[k])
		   || fbits(p->vel[k]) != fbits(e->vel[k]))
			return false;
	return true;
}
static volatile int g_drain_run = 1;
static int g_failures = 0;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

#define CHECK(cond, ...)                                                       \
	do {                                                                       \
		if(!(cond)) {                                                          \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);                      \
			printf(__VA_ARGS__);                                               \
			printf("\n");                                                      \
			g_failures++;                                                      \
		}                                                                      \
	} while(0)


static void set_block(int x, int y, int z, int type) {
	struct block_data cur;
	if(server_world_get_block(&s.world, x, y, z, &cur) && cur.type == type
	   && cur.metadata == 0)
		return; // already there (a no-op write is refused)
	bool ok = server_world_set_block(&s.worlds[0], x, y, z, (struct block_data) {.type = (uint8_t)type});
	assert(ok);
}

static struct entity* make_item_id(uint32_t id, float x, float y, float z) {
	struct entity* e = malloc(sizeof *e);
	assert(e);
	memset(e, 0, sizeof *e);
	entity_item(id, e, true, &s.world,
				(struct item_data) {.id = BLOCK_COBBLESTONE, .count = 1});
	e->teleport(e, (vec3) {x, y, z});
	glm_vec3_zero(e->vel);
	e->data.item.was_in_water = false;
	return e;
}

static struct entity* make_item(float x, float y, float z) {
	return make_item_id(900, x, y, z);
}

/* Actual JRE java.util.Random + JVM strict float operations, seeds 42/12345.
 * push = nextFloat()*0.2F+0.1F; dragged = push*0.98F [Entity.java:1220]. */
static const struct {
	uint32_t dragged_bits;
	uint64_t state;
} push_oracle[2][4] = {
	{{0x3e766081U, UINT64_C(204790973191750)},
	 {0x3ddea5a6U, UINT64_C(15386904305625)},
	 {0x3e6d7a49U, UINT64_C(192310312797552)},
	 {0x3ddbf27eU, UINT64_C(13493716152507)}},
	{{0x3e2cf7a3U, UINT64_C(101838520498639)},
	 {0x3e4b5aefU, UINT64_C(144455641405230)},
	 {0x3e8fcdc4U, UINT64_C(262614317937313)},
	 {0x3e8e35d6U, UINT64_C(258144839677080)}}
};

/* Two consecutive lava ticks, seed 42, four ordered floats per tick.
 * Exact output + cursor catches swapped pairs, reversed subtraction,
 * skipped/extra draws and per-tick reseeding. [EntityItem.java:67-68] */
static const struct {
	uint32_t x_bits, z_bits, dragged_x_bits, dragged_z_bits;
	uint64_t state;
} lava_oracle[2] = {
	{0x3e09cf43U, 0x3e021b2fU, 0x3e070dadU, 0x3dff0215U,
	 UINT64_C(13493716152507)},
	{0xbe01b5ffU, 0xbdb0630dU, 0xbdfe3bc1U, 0xbdacdbf3U,
	 UINT64_C(199202821746287)}
};

static float from_bits(uint32_t bits) {
	float f;
	memcpy(&f, &bits, sizeof f);
	return f;
}

static void check_gameplay_rng(const struct entity* e, uint64_t seed,
							 bool cached, double cache) {
	CHECK(e->gameplay_rng.seed == seed, "item %u gameplay cursor %llu want %llu",
		  e->id, (unsigned long long)e->gameplay_rng.seed,
		  (unsigned long long)seed);
	CHECK(e->gameplay_rng.have_next_gaussian == cached
		  && e->gameplay_rng.next_gaussian_cached == cache,
		  "item %u float draws retain Gaussian cache", e->id);
}

/* Tick until destroyed; returns the tick number (1-based) or 0. */
static int ticks_until_destroyed(struct entity* e, int max, short* hp_trace) {
	for(int t = 1; t <= max; t++) {
		bool gone = e->tick_server(e, &s);
		if(hp_trace)
			hp_trace[t] = e->health;
		if(gone)
			return t;
	}
	return 0;
}

static void test_ice_friction(void) {
	printf("1. ice / stone ground friction (EntityItem.java:55-66)\n");
	struct entity* ice = make_item(3.5F, 101.125F, 4.5F);
	struct entity* stone = make_item(3.5F, 101.125F, 8.5F);
	ice->vel[0] = 0.1F;
	stone->vel[0] = 0.1F;
	uint32_t seed0 = s.rand_src.seed;

	static const struct {
		int t;
		double ice_x, ice_v, stone_x, stone_v;
	} g[] = {
		{1, 3.600000001, 0.0960400059, 3.600000001, 0.0588000068},
		{10, 4.339373985, 0.0667608294, 3.741519327, 0.000494053186},
		{30, 5.273856684, 0.0297553565, 3.742718456, 1.20592722e-08},
	};
	size_t gi = 0;
	for(int t = 1; t <= 30; t++) {
		ice->tick_server(ice, &s);
		stone->tick_server(stone, &s);
		if(t == 1) {
			CHECK(fbits(ice->vel[0]) == fbits(0.1F * (0.98F * 0.98F)),
				  "ice t1 vel bits %08x want %08x", fbits(ice->vel[0]),
				  fbits(0.1F * (0.98F * 0.98F)));
			CHECK(fbits(stone->vel[0]) == fbits(0.1F * (0.6F * 0.98F)),
				  "stone t1 vel bits %08x", fbits(stone->vel[0]));
			CHECK(ice->on_ground && stone->on_ground, "resting items grounded");
		}
		if(gi < sizeof g / sizeof g[0] && g[gi].t == t) {
			CHECK(fabs(ice->pos[0] - g[gi].ice_x) < 2e-5,
				  "ice t%d x %.7f want %.7f", t, ice->pos[0], g[gi].ice_x);
			CHECK(fabs(ice->vel[0] - g[gi].ice_v) < 1e-6 * 10,
				  "ice t%d v %.9g want %.9g", t, ice->vel[0], g[gi].ice_v);
			CHECK(fabs(stone->pos[0] - g[gi].stone_x) < 2e-5,
				  "stone t%d x %.7f want %.7f", t, stone->pos[0],
				  g[gi].stone_x);
			CHECK(fabs(stone->vel[0] - g[gi].stone_v)
					  < 1e-6 * fabs(g[gi].stone_v) + 1e-12,
				  "stone t%d v %.9g want %.9g (no 0.005 snap in Beta)", t,
				  stone->vel[0], g[gi].stone_v);
			gi++;
		}
		CHECK(ice->vel[1] == 0.0F, "ice t%d resting vel.y %g", t, ice->vel[1]);
	}
	CHECK(s.rand_src.seed == seed0, "no rand_src draw off lava");
	printf("  ice x30=%.6f v30=%.9g stone x30=%.6f\n", ice->pos[0],
		   ice->vel[0], stone->pos[0]);
	free(ice);
	free(stone);
}

static void test_fall_landing(void) {
	printf("2+3. airborne fall order + landing, no rebound "
		   "(EntityItem.java:45,54,65-69; Entity.java:424-426)\n");
	struct entity* e = make_item(8.5F, 110.125F, 20.5F);
	e->vel[1] = 0.2F; // EntityItem ctor: initial motionY (double)0.2F
	static const double beta_y[28] = {
		0, 110.285000004, 110.401800012, 110.476264022, 110.509238755,
		110.501553995, 110.454022930, 110.367442487, 110.242593652,
		110.080241793, 109.881136968, 109.646014237, 109.375593956,
		109.070582078, 108.731670431, 108.359537013, 107.954846256,
		107.518249308, 107.050384291, 106.551876566, 106.023338988,
		105.465372152, 104.878564642, 104.263493273, 103.620723320,
		102.950808755, 102.254292469, 101.531706497};
	for(int t = 1; t <= 27; t++) {
		e->tick_server(e, &s);
		CHECK(fabs(e->pos[1] - beta_y[t]) < 2e-4, "fall t%d y %.6f want %.6f",
			  t, e->pos[1], beta_y[t]);
		CHECK(!e->on_ground, "fall t%d airborne", t);
	}
	e->tick_server(e, &s); // tick 28: Beta lands (minY clipped to 101.0)
	CHECK(e->on_ground, "t28 landed");
	CHECK(e->vel[1] == 0.0F, "t28 vel.y %g (Beta: 0 * 0.98 * -0.5)",
		  e->vel[1]);
	CHECK(e->pos[1] >= 101.125F - 1e-4F && e->pos[1] < 101.125F + 0.011F,
		  "t28 rests on the floor face, y %.6f", e->pos[1]);
	float prev = e->pos[1];
	for(int t = 29; t <= 34; t++) {
		e->tick_server(e, &s);
		CHECK(e->on_ground && e->vel[1] == 0.0F && e->pos[1] <= prev,
			  "t%d no rebound: y %.6f vy %g", t, e->pos[1], e->vel[1]);
		prev = e->pos[1];
	}
	free(e);
}

static void test_lava(void) {
	printf("4. lava: bounce velocity, RNG draws, destroyed on the Beta tick "
		   "(EntityItem.java:46-51; Entity.java:218-240,480-491)\n");
	/* (a) minY 101.375: expand(-0.1,-0.4,-0.1) scans y=101 -> -4 in
	 * onEntityUpdate, then the post-move burn -1: dead on tick 1. */
	struct entity* a = make_item(27.5F, 101.5F, 27.5F);
	jrand_set_seed(&a->gameplay_rng, INT64_C(42));
	a->gameplay_rng.have_next_gaussian = true;
	a->gameplay_rng.next_gaussian_cached = 0.375;
	uint32_t shared0 = s.entity_rng.seed;
	uint32_t world0 = s.rand_src.seed;
	struct random_gen snd = *server_local_sound_rng();
	(void)rand_gen_flt(&snd); // fizz pitch 2.0F + nextFloat()*0.4F
	int dead = ticks_until_destroyed(a, 1, NULL);
	CHECK(dead == 1, "lava(a) destroyed on tick 1 (got %d)", dead);
	CHECK(a->health == 0, "lava(a) health %d want 5-4-1=0", a->health);
	check_gameplay_rng(a, lava_oracle[0].state, true, 0.375);
	CHECK(s.entity_rng.seed == shared0, "lava(a) shared entity_rng untouched");
	CHECK(s.rand_src.seed == world0, "lava(a) rand_src (World.rand) untouched");
	CHECK(server_local_sound_rng()->seed == snd.seed,
		  "lava(a) exactly 1 sound draw");
	float mx = from_bits(lava_oracle[0].x_bits);
	float mz = from_bits(lava_oracle[0].z_bits);
	CHECK(fbits(a->vel[1]) == 0x3e48b43aU, "lava(a) vel.y bits %08x",
		  fbits(a->vel[1]));
	CHECK(fbits(a->vel[0]) == lava_oracle[0].dragged_x_bits,
		  "lava(a) ordered r1-r2 vel.x bits %08x", fbits(a->vel[0]));
	CHECK(fbits(a->vel[2]) == lava_oracle[0].dragged_z_bits,
		  "lava(a) ordered r3-r4 vel.z bits %08x", fbits(a->vel[2]));
	CHECK(fabsf(a->pos[1] - 101.7F) < 1e-5F, "lava(a) rose 0.2: %.6f",
		  a->pos[1]);
	CHECK(fabsf(a->pos[0] - (27.5F + mx)) < 1e-5F, "lava(a) x nudged");
	CHECK(fabsf(a->pos[2] - (27.5F + mz)) < 1e-5F, "lava(a) z nudged");
	CHECK(a->data.item.persisted_fire == 601, "lava(a) fire %d want 601",
		  a->data.item.persisted_fire);
	free(a);

	/* (b) minY 101.01: tick 1 expanded box is empty (burn only, 5->4,
	 * fire 0->1); risen to minY 101.21, tick 2 expanded box hits: -4 = dead. */
	struct entity* b = make_item(27.5F, 101.135F, 27.5F);
	jrand_set_seed(&b->gameplay_rng, INT64_C(42));
	b->gameplay_rng.have_next_gaussian = true;
	b->gameplay_rng.next_gaussian_cached = 0.625;
	snd = *server_local_sound_rng();
	(void)rand_gen_flt(&snd);
	(void)rand_gen_flt(&snd);
	g_corr.count = 0;
	bool gone1 = b->tick_server(b, &s);
	CHECK(!gone1 && b->health == 4, "lava(b) t1 alive, health %d want 4",
		  b->health);
	CHECK(fbits(b->vel[0]) == lava_oracle[0].dragged_x_bits
		  && fbits(b->vel[2]) == lava_oracle[0].dragged_z_bits,
		  "lava(b) t1 four ordered gameplay floats");
	check_gameplay_rng(b, lava_oracle[0].state, true, 0.625);
	CHECK(g_corr.count == 1 && correction_matches(b),
		  "lava(b) t1 bounce sends CRPC_ENTITY_PHYSICS_CORRECTION with the "
		  "server pos/vel (count %d)", g_corr.count);
	short hp[4] = {0};
	dead = ticks_until_destroyed(b, 2, hp);
	if(dead)
		dead += 1;
	CHECK(dead == 2, "lava(b) destroyed on tick 2 (got %d)", dead);
	CHECK(fbits(b->vel[0]) == lava_oracle[1].dragged_x_bits
		  && fbits(b->vel[2]) == lava_oracle[1].dragged_z_bits,
		  "lava(b) t2 consumes next four floats, no reseed");
	check_gameplay_rng(b, lava_oracle[1].state, true, 0.625);
	CHECK(s.entity_rng.seed == shared0, "lava(b) shared entity_rng untouched");
	CHECK(s.rand_src.seed == world0, "lava(b) rand_src untouched");
	CHECK(server_local_sound_rng()->seed == snd.seed,
		  "lava(b) exactly one sound draw per lava tick");
	free(b);
}

static void test_fire(void) {
	printf("5. fire block contact (Entity.java:203-216,480-491; "
		   "EntityItem.java:83-95)\n");
	/* Fresh item, fire starts 0: burn -1/tick, fire 0->1->0->1: tick 5. */
	struct entity* a = make_item(5.5F, 101.125F, 26.5F);
	short hp[8] = {0};
	int dead = ticks_until_destroyed(a, 7, hp);
	CHECK(hp[1] == 4 && hp[2] == 3 && hp[3] == 2 && hp[4] == 1,
		  "fire(a) health %d %d %d %d want 4 3 2 1", hp[1], hp[2], hp[3],
		  hp[4]);
	CHECK(dead == 5, "fresh item in fire destroyed on tick 5 (got %d)", dead);
	free(a);

	/* One tick on plain stone: fire <= 0 -> fire = -fireResistance = -1. */
	struct entity* b = make_item(14.5F, 101.125F, 26.5F);
	b->tick_server(b, &s);
	CHECK(b->data.item.persisted_fire == -1, "off-fire fire %d want -1",
		  b->data.item.persisted_fire);
	b->teleport(b, (vec3) {11.5F, 101.125F, 26.5F});
	short hp2[8] = {0};
	dead = ticks_until_destroyed(b, 7, hp2);
	CHECK(hp2[1] == 4 && hp2[2] == 2, "fire(b) health %d %d want 4 2", hp2[1],
		  hp2[2]);
	CHECK(dead == 3, "fire=-1 item destroyed on tick 3 (got %d)", dead);
	free(b);
}

static void test_void_kill(void) {
	printf("6. void kill: posY < -64 -> kill() before the move "
		   "(Entity.java:222-224)\n");
	struct entity* a = make_item(8.5F, -64.5F, 20.5F);
	int dead = ticks_until_destroyed(a, 3, NULL);
	CHECK(dead == 1 && a->health == 5, "posY -64.5 removed on tick 1 (%d)",
		  dead);
	free(a);
	/* -63.99 is not < -64 on tick 1; it falls 0.04 to -64.03 and the tick-2
	 * onEntityUpdate check removes it. */
	struct entity* b = make_item(8.5F, -63.99F, 20.5F);
	dead = ticks_until_destroyed(b, 3, NULL);
	CHECK(dead == 2, "posY -63.99 removed on tick 2 (got %d)", dead);
	free(b);
}

static void test_push_out(void) {
	printf("7. pushOutOfBlocks on owned Java48 (Entity.java:1220)\n");
	uint32_t world0 = s.rand_src.seed;
	uint32_t shared0 = s.entity_rng.seed;
	uint32_t sound0 = server_local_sound_rng()->seed;
	/* (i) isolated stone cube, center at fy 0.8: +Y (0.2) is nearest. */
	struct entity* a = make_item(20.5F, 110.8F, 12.5F);
	/* Behavioral RED against preimage: the REAL callback used s.entity_rng.
	 * Seed injection is only a behavior control; constructors are tested
	 * independently in test_item_entity_parity and persistence_env. */
	jrand_set_seed(&a->gameplay_rng, INT64_C(42));
	g_corr.count = 0;
	a->tick_server(a, &s);
	CHECK(fbits(a->vel[1]) == 0x3e766081U,
		  "RED witness: seed42 +Y vel.y bits %08x want JRE 3e766081",
		  fbits(a->vel[1]));
	CHECK(a->vel[0] == 0.0F && a->vel[2] == 0.0F,
		  "(i) nearest +Y preserves X/Z");
	check_gameplay_rng(a, push_oracle[0][0].state, false, 0);
	CHECK(s.entity_rng.seed == shared0, "(i) shared entity_rng untouched");
	CHECK(g_corr.count == 1 && correction_matches(a), "(i) correction sent");
	CHECK(fbits(g_corr.last.payload.entity_physics_correction.vel[1])
		  == 0x3e766081U, "(i) actual correction carries JRE vel.y bits");
	free(a);

	/* (ii) center of the cube: all six distances 0.5 -> -X wins the tie. */
	struct entity* b = make_item(20.5F, 110.5F, 12.5F);
	jrand_set_seed(&b->gameplay_rng, INT64_C(42));
	g_corr.count = 0;
	b->tick_server(b, &s);
	CHECK(fbits(b->vel[0]) == (push_oracle[0][0].dragged_bits | 0x80000000U)
			  && fbits(b->vel[1]) == fbits(-0.04F * 0.98F) && b->vel[2] == 0.0F,
		  "(ii) tie -> -X: vel %g %g %g want %g %g 0", b->vel[0], b->vel[1],
		  b->vel[2], -from_bits(push_oracle[0][0].dragged_bits), -0.04F * 0.98F);
	check_gameplay_rng(b, push_oracle[0][0].state, false, 0);
	CHECK(s.entity_rng.seed == shared0, "(ii) shared entity_rng untouched");
	CHECK(g_corr.count == 1 && correction_matches(b), "(ii) correction sent");
	free(b);

	/* (iii) sealed cube (all six faces stone): the roll is drawn, no axis. */
	struct entity* c = make_item(24.5F, 110.5F, 12.5F);
	jrand_set_seed(&c->gameplay_rng, INT64_C(42));
	g_corr.count = 0;
	c->tick_server(c, &s);
	CHECK(c->vel[0] == 0.0F && c->vel[2] == 0.0F
			  && fbits(c->vel[1]) == fbits(-0.04F * 0.98F),
		  "(iii) sealed: vel %g %g %g", c->vel[0], c->vel[1], c->vel[2]);
	check_gameplay_rng(c, push_oracle[0][0].state, false, 0);
	CHECK(s.entity_rng.seed == shared0, "(iii) shared entity_rng untouched");
	CHECK(g_corr.count == 0, "(iii) no correction when nothing moved");
	free(c);
	/* (iv) no embedded block: no gameplay draw and no push correction. */
	struct entity* d = make_item(18.5F, 110.8F, 12.5F);
	jrand_set_seed(&d->gameplay_rng, INT64_C(42));
	g_corr.count = 0;
	d->tick_server(d, &s);
	CHECK(d->vel[0] == 0.0F && d->vel[2] == 0.0F
		  && fbits(d->vel[1]) == fbits(-0.04F * 0.98F),
		  "(iv) no embedded block: gravity only");
	check_gameplay_rng(d, UINT64_C(25214903879), false, 0);
	CHECK(g_corr.count == 0, "(iv) no correction without embedding");
	free(d);
	CHECK(s.rand_src.seed == world0, "pushOut never touches rand_src");
	CHECK(s.entity_rng.seed == shared0, "pushOut never touches shared entity_rng");
	CHECK(server_local_sound_rng()->seed == sound0, "pushOut never draws sound");
}

static void test_item_rng_interleaved(void) {
	printf("7b. distinct IDs, interleaved push ticks retain independent Java48\n");
	struct entity* a = make_item_id(901, 20.5F, 110.8F, 12.5F);
	struct entity* b = make_item_id(902, 20.5F, 110.8F, 12.5F);
	CHECK(a->id != b->id, "independent lifetimes have distinct assigned IDs");
	jrand_set_seed(&a->gameplay_rng, INT64_C(42));
	jrand_set_seed(&b->gameplay_rng, INT64_C(12345));
	a->gameplay_rng.have_next_gaussian = true;
	a->gameplay_rng.next_gaussian_cached = 0.375;
	b->gameplay_rng.have_next_gaussian = true;
	b->gameplay_rng.next_gaussian_cached = -0.625;
	uint32_t world0 = s.rand_src.seed, shared0 = s.entity_rng.seed;
	uint32_t sound0 = server_local_sound_rng()->seed;
	struct entity* items[2] = {a, b};
	/* Change relative order each round; compare each owner against the JRE's
	 * isolated sequence. Teleport resets position only, never the RNG. */
	for(int draw = 0; draw < 4; draw++) {
		for(int turn = 0; turn < 2; turn++) {
			int owner = (draw + turn) % 2;
			struct entity* e = items[owner];
			struct entity* other = items[1 - owner];
			struct java_random untouched = other->gameplay_rng;
			e->teleport(e, (vec3) {20.5F, 110.8F, 12.5F});
			glm_vec3_zero(e->vel);
			g_corr.count = 0;
			CHECK(!e->tick_server(e, &s), "interleaved item remains alive");
			CHECK(fbits(e->vel[1]) == push_oracle[owner][draw].dragged_bits,
				  "item %u draw %d vel.y bits %08x want %08x", e->id,
				  draw + 1, fbits(e->vel[1]), push_oracle[owner][draw].dragged_bits);
			check_gameplay_rng(e, push_oracle[owner][draw].state, true,
							   owner == 0 ? 0.375 : -0.625);
			CHECK(other->gameplay_rng.seed == untouched.seed
				  && other->gameplay_rng.have_next_gaussian == untouched.have_next_gaussian
				  && other->gameplay_rng.next_gaussian_cached == untouched.next_gaussian_cached,
				  "ticking item %u never perturbs item %u", e->id, other->id);
			CHECK(g_corr.count == 1 && correction_matches(e),
				  "interleaved item correction matches exact server state");
			CHECK(s.rand_src.seed == world0 && s.entity_rng.seed == shared0,
				  "interleaved tick preserves fixture world/shared streams");
		}
	}
	CHECK(server_local_sound_rng()->seed == sound0, "interleaved push has no sound draw");
	free(a);
	free(b);
}

/* The pre-W1C-b burn probe, verbatim: dict-scan preflight over Beta's
 * inclusive chunk span, then the half-open cell scan. */
static bool burning_reference(struct entity* e) {
	struct AABB box;
	entity_make_world_bbox(e, &box);
	const w_coord_t x0 = (w_coord_t)floor((double)box.x1 + 0.001);
	const w_coord_t x1 = (w_coord_t)floor((double)box.x2 - 0.001 + 1.0);
	const w_coord_t y0 = (w_coord_t)floor((double)box.y1 + 0.001);
	const w_coord_t y1 = (w_coord_t)floor((double)box.y2 - 0.001 + 1.0);
	const w_coord_t z0 = (w_coord_t)floor((double)box.z1 + 0.001);
	const w_coord_t z1 = (w_coord_t)floor((double)box.z2 - 0.001 + 1.0);
	if(y1 < 0 || y0 >= WORLD_HEIGHT)
		return false;
	for(w_coord_t cx = WCOORD_CHUNK_OFFSET(x0); cx <= WCOORD_CHUNK_OFFSET(x1);
		cx++)
		for(w_coord_t cz = WCOORD_CHUNK_OFFSET(z0);
			cz <= WCOORD_CHUNK_OFFSET(z1); cz++)
			if(!server_world_is_chunk_loaded(&s.world, cx, cz))
				return false;
	for(w_coord_t x = x0; x < x1; x++)
		for(w_coord_t y = y0; y < y1; y++)
			for(w_coord_t z = z0; z < z1; z++) {
				struct block_data b;
				if(server_world_get_block(&s.world, x, y, z, &b)
				   && (b.type == BLOCK_FIRE || b.type == BLOCK_LAVA_FLOW
					   || b.type == BLOCK_LAVA_STILL))
					return true;
			}
	return false;
}

static void test_burn_probe_equiv(void) {
	printf("8. T1 cached burn probe == dict-scan original (seams incl. an "
		   "unloaded chunk)\n");
	struct entity* e = make_item(0, 0, 0);
	int n = 0, hits = 0, mismatches = 0;
	/* x sweep across the loaded 15|16 seam and the loaded|UNLOADED 63|64
	 * seam; z sweep across 63|64; y through the fire/lava layer. */
	for(int pass = 0; pass < 2; pass++)
		for(float a = 12.0F; a < 64.6F; a += 0.0625F)
			for(float y = 100.9F; y < 102.4F; y += 0.1F) {
				vec3 p = {pass ? 40.5F : a, y, pass ? a : 40.5F};
				e->teleport(e, p);
				bool want = burning_reference(e);
				bool got = entity_item_bounding_box_burning(&s, e);
				n++;
				hits += want;
				if(want != got && mismatches++ < 5)
					printf("  mismatch at %.4f %.4f %.4f ref %d got %d\n",
						   p[0], p[1], p[2], want, got);
			}
	CHECK(mismatches == 0 && hits > 0, "burn probe equivalence: %d/%d "
		  "positions differ (%d burning)", mismatches, n, hits);
	printf("  %d positions, %d burning, 0 differ\n", n, hits);

	/* Per-tick cost of one resting item (debug-overlay instruments). */
	struct entity* r = make_item(8.5F, 101.125F, 12.5F);
	r->tick_server(r, &s);
	server_world_perf_tick_reset();
	r->tick_server(r, &s);
	server_world_perf_tick_reset();
	int us, nset, maxus, nget, maxget;
	server_world_edit_stats(&us, &nset, &maxus, &nget, &maxget);
	printf("  resting item: %d server_world_get_block calls per tick, chunk "
		   "cache hit %d%%\n", nget, server_world_chunkres_hit_pct());
	free(r);
	free(e);
}

int main(void) {
	test_fixture_dir("test_item_env_parity_golden_tmp", s_fixture_TMP_WORLD,
					 sizeof s_fixture_TMP_WORLD);
	assert(clin_init());
	blocks_init();
	items_init();

	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));

	string_t level;
	string_init_set_str(level, TMP_WORLD);

	memset(&s, 0, sizeof(s));
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&s))
		abort();
	piston_moving_store_init(&s.piston_moving);
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.world.local = &s;
	s.scheduled_tick_budget = 128;
	rand_gen_seed(&s.rand_src);
	s.entity_rng.seed = 0x13579BDFu; // sentinel: item rolls must not hit this stream
	dict_entity_init(s.entities);
	clin_test_set_rpc_observer(observe_rpc, NULL);
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 60.5; // far from every test item: no pickup, no splash
	s.player.y = 140.0;
	s.player.z = 60.5;

	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));

	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 3; cx++)
		for(int cz = 0; cz <= 3; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	for(int x = 0; x <= 31; x++)
		for(int z = 0; z <= 31; z++) {
			set_block(x, 100, z, BLOCK_STONE);
			for(int y = 101; y <= 115; y++)
				set_block(x, y, z, BLOCK_AIR);
		}
	for(int x = 1; x <= 30; x++)
		set_block(x, 100, 4, BLOCK_ICE);
	for(int x = 26; x <= 28; x++)
		for(int z = 26; z <= 28; z++)
			for(int y = 101; y <= 103; y++)
				set_block(x, y, z, BLOCK_LAVA_STILL);
	set_block(5, 101, 26, BLOCK_FIRE);
	set_block(11, 101, 26, BLOCK_FIRE);
	set_block(20, 110, 12, BLOCK_STONE); // pushOut (i)/(ii): isolated cube
	set_block(24, 110, 12, BLOCK_STONE); // pushOut (iii): sealed cube
	set_block(23, 110, 12, BLOCK_STONE);
	set_block(25, 110, 12, BLOCK_STONE);
	set_block(24, 109, 12, BLOCK_STONE);
	set_block(24, 111, 12, BLOCK_STONE);
	set_block(24, 110, 11, BLOCK_STONE);
	set_block(24, 110, 13, BLOCK_STONE);
	set_block(15, 101, 40, BLOCK_FIRE); // burn-probe seams
	set_block(16, 101, 40, BLOCK_LAVA_STILL);
	set_block(63, 101, 40, BLOCK_FIRE);
	set_block(40, 101, 63, BLOCK_FIRE);
	set_block(40, 101, 16, BLOCK_LAVA_STILL);

	uint32_t fixture_world0 = s.rand_src.seed;
	uint32_t fixture_shared0 = s.entity_rng.seed;
	printf("test_item_env_parity_golden:\n");
	test_ice_friction();
	test_fall_landing();
	test_lava();
	test_fire();
	test_void_kill();
	test_push_out();
	test_item_rng_interleaved();
	test_burn_probe_equiv();
	CHECK(s.rand_src.seed == fixture_world0
		  && s.entity_rng.seed == fixture_shared0,
		  "all item fixture callbacks preserve world/shared entity RNG streams");
	clin_test_set_rpc_observer(NULL, NULL);

	g_drain_run = 0;
	thread_join(&drain);
	if(g_failures) {
		printf("test_item_env_parity_golden: %d FAILURE(S)\n", g_failures);
		return 1;
	}
	printf("test_item_env_parity_golden: ALL PASS\n");
	return 0;
}
