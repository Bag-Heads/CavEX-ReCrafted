/*
	Deterministic, headless test of the dungeon mob-spawner's burst core
	(source/network/spawner.c) -- the RNG-driven spot selection that the live PC
	verification could only exercise through a flaky GUI. No client / RPC / entity
	objects: the block accessor is injected, pointing at a flat test world.

	Proves the behaviour the spawner is written for (a floor-mounted dungeon
	spawner):
	  - spawner_spot_ok accepts a 2-air gap on a solid floor and rejects air-only
	    columns, solid cells, and out-of-world y;
	  - spawner_mob_from_meta maps Beta's pickMobSpawner index to the engine ids
	    (FH-17: migration-only now -- see spawner_mob_to_id/spawner_mob_from_id);
	  - FH-17: spawner_mob_to_id maps the same index to Beta's exact EntityId
	    string, and spawner_mob_from_id resolves that string back to the engine
	    id (the runtime path server_local_tick_spawners now uses, reading a
	    MobSpawner tile entity instead of block metadata) -- including the
	    "inert" contract (blank/NULL/unrecognised EntityId -> -1, no spawn, no
	    crash, matching Beta's own createEntityInWorld(null) no-op);
	  - spawner_burst, fired on a flat floor, ACTUALLY emits spawns (the live
	    0-fire was an awkward-terrain harness artifact, not this logic), and every
	    emitted spot is itself a valid mob spot;
	  - spawner_hostile_light_ok (2026-08-31 fix) implements Beta EntityMob.
	    getCanSpawnHere()'s exact TWO-ROLL light gate -- open-sky suppression via
	    the raw/unadjusted sky light (roll 1), then the smoothly-graded
	    day/night-adjusted effective-light roll (roll 2), NOT the flat `eff<=7`
	    cutoff the dungeon-spawner path used to skip entirely -- with exact
	    boundary + short-circuit behaviour pinned via a scripted RNG.
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks.h"			// struct block, blocks[]
#include "block/blocks_data.h"		// BLOCK_*, w_coord_t, struct block_data
#include "entity/monsters_object.h" // MONSTER_*
#include "network/spawner.h"
#include "util.h" // struct random_gen + the rand_gen_* contract

// --- minimal block table ----------------------------------------------------
// These stubs MUST mirror how real blocks are declared, or this test is worse
// than useless. It previously used `{.opacity = 15}`, which NO real block sets:
// block_stone.c / block_grass.c / block_dirt.c / block_cobblestone.c declare only
// .transparent and .can_see_through, leaving the .opacity bitfield at its 0
// default. That stub satisfied BOTH the broken `opacity >= 15` floor predicate
// and the correct one, so this test stayed green while dungeon mob spawners were
// 100% dead on every real cobblestone floor (found by adversarial review
// 2026-07-31, fixed in the same batch). Declare stubs the way the registry does.
static struct block b_solid = {
	.transparent = false,
	.can_see_through = false, // <- what actually makes it a normal cube
};
// A snow LAYER: not a full cube, so never a floor -- the other half of the
// predicate, and the block that made the maintainer's whole world unspawnable.
static struct block b_layer = {
	.transparent = false,
	.can_see_through = true,
};
struct block* blocks[256] = {0};

// Mirror of blocks.c:47 block_is_normal_cube (Beta World.isBlockNormalCube).
// spawner.c calls the canonical helper; linking the real blocks.c would drag in
// the entire block registry for a test about spawn geometry, so mirror it here.
// If blocks.c:47 changes, change this.
bool block_is_normal_cube(uint8_t type) {
	const struct block* b = blocks[type];
	return b && !b->can_see_through && type != BLOCK_TNT;
}

// --- a self-contained RNG satisfying util.h (we don't link util.c). The spot
//     logic does not depend on the exact sequence, only that offsets fall in the
//     +-spread range, so any in-contract generator exercises it. ---------------
uint32_t rand_gen(struct random_gen* g) {
	g->seed = g->seed * 1103515245u + 12345u;
	return g->seed;
}
float rand_gen_flt(struct random_gen* g) {
	return (float)(rand_gen(g) >> 8) / (float)(1u << 24); // [0,1)
}
// Scripted-draw mode for section (4): spawner_hostile_light_ok's exact two-roll
// SHAPE (short-circuit on roll 1, the roll-2 boundary, thunder-forced subtract)
// needs an EXACT, known draw sequence -- the plain LCG below only guarantees a
// value falls in range, not which one. When g_script_active is set, rand_gen_int
// replays g_script[] in order instead of drawing from the LCG; section (3)'s
// geometric burst test never sets it, so its behaviour is unchanged.
static bool g_script_active = false;
static int g_script[4];
static int g_script_n = 0, g_script_i = 0;
static void script_set(int a) {
	g_script_active = true;
	g_script[0] = a;
	g_script_n = 1;
	g_script_i = 0;
}
static void script_set2(int a, int b) {
	g_script_active = true;
	g_script[0] = a;
	g_script[1] = b;
	g_script_n = 2;
	g_script_i = 0;
}
int rand_gen_int(struct random_gen* g, int max) {
	if(g_script_active) {
		assert(g_script_i < g_script_n && "scripted RNG over-drawn");
		int v = g_script[g_script_i++];
		assert(v >= 0 && v < max && "scripted value out of the requested range");
		return v;
	}
	return (int)(rand_gen(g) % (uint32_t)max); // [0,max)
}

// --- flat world: solid floor at y<=FLOOR, air above ---------------------------
#define FLOOR 64
#define WH 128
static bool flat_get(void* ctx, w_coord_t x, w_coord_t y, w_coord_t z,
					 struct block_data* blk) {
	(void)ctx;
	(void)x;
	(void)z;
	if(y < 0 || y >= WH)
		return false; // out of world
	memset(blk, 0, sizeof(*blk));
	blk->type = (y <= FLOOR) ? BLOCK_STONE : BLOCK_AIR;
	return true;
}

// record emitted spawn spots
static struct {
	double x, y, z;
} g_rec[2048];
static int g_nrec;
/* I04: the member light rolls now come from the member Random (the
 * production caller passes the candidate lifetime stream); a test stream. */
static struct random_gen* g_member_rng;
static bool test_can_spawn(void* ctx, int raw_sky_light, int block_light_value) {
	(void)ctx;
	return spawner_hostile_light_value_ok(g_member_rng, raw_sky_light,
										  block_light_value);
}

static void rec_emit(void* ctx, double x, double y, double z) {
	(void)ctx;
	if(g_nrec < (int)(sizeof(g_rec) / sizeof(g_rec[0]))) {
		g_rec[g_nrec].x = x;
		g_rec[g_nrec].y = y;
		g_rec[g_nrec].z = z;
		g_nrec++;
	}
}

/* (6) hooks: count constructs; the crowd is full from member g_crowd_from. */
static int g_constructs, g_crowd_from;
static void count_construct(void* ctx) {
	(void)ctx;
	g_constructs++;
}
static bool crowd_full(void* ctx) {
	(void)ctx;
	return g_constructs - 1 >= g_crowd_from;
}

int main(void) {
	blocks[BLOCK_STONE] = &b_solid;

	// (1) spot validity on a flat floor.
	assert(spawner_spot_ok(flat_get, NULL, 5, FLOOR + 1, 7)); // air/air/solid
	assert(!spawner_spot_ok(flat_get, NULL, 5, FLOOR, 7));	 // here is solid
	assert(!spawner_spot_ok(flat_get, NULL, 5, FLOOR + 2, 7)); // no floor below
	assert(!spawner_spot_ok(flat_get, NULL, 5, 0, 7));		  // y < 1

	// (2) Beta pickMobSpawner index -> engine monster id (migration-only path).
	assert(spawner_mob_from_meta(0) == MONSTER_SKELETON);
	assert(spawner_mob_from_meta(1) == MONSTER_ZOMBIE);
	assert(spawner_mob_from_meta(2) == MONSTER_ZOMBIE);
	assert(spawner_mob_from_meta(3) == MONSTER_SPIDER);
	// metadata is masked &3 -- an out-of-range raw byte still resolves.
	assert(spawner_mob_from_meta(4) == MONSTER_SKELETON); // 4&3 == 0
	assert(spawner_mob_from_meta(255) == MONSTER_SPIDER); // 255&3 == 3

	// (2b) FH-17: the SAME pickMobSpawner index -> the exact Beta EntityId
	// string (dungeon worldgen's materialisation target, and the migration's
	// synthesis source).
	assert(strcmp(spawner_mob_to_id(0), "Skeleton") == 0);
	assert(strcmp(spawner_mob_to_id(1), "Zombie") == 0);
	assert(strcmp(spawner_mob_to_id(2), "Zombie") == 0);
	assert(strcmp(spawner_mob_to_id(3), "Spider") == 0);
	assert(spawner_mob_to_id(4) == NULL); // out of range, never masked here

	// (2c) FH-17: the runtime path -- Beta EntityId string -> engine monster
	// id, the resolver server_local_tick_spawners now calls instead of
	// spawner_mob_from_meta. Round-trips with (2b) for all 3 dungeon types.
	assert(spawner_mob_from_id("Skeleton") == MONSTER_SKELETON);
	assert(spawner_mob_from_id("Zombie") == MONSTER_ZOMBIE);
	assert(spawner_mob_from_id("Spider") == MONSTER_SPIDER);
	// Beta's own "inert" states: absent tag ("" default), NULL, and any
	// EntityId this engine's spawner path does not implement all resolve to
	// -1, matching Beta's createEntityInWorld(mobID) returning null (no
	// spawn, no crash) rather than crashing or guessing a fallback mob.
	assert(spawner_mob_from_id("") == -1);
	assert(spawner_mob_from_id(NULL) == -1);
	assert(spawner_mob_from_id("Pig") == -1);
	assert(spawner_mob_from_id("Creeper") == -1);
	assert(spawner_mob_from_id("skeleton") == -1); // case-sensitive, like Beta's String.equals

	// (3) A floor-mounted spawner DOES emit mobs, and every emitted spot is a
	// valid mob spot. This is the proof the live GUI run could not capture.
	// sky_subtract=0: flat_get zeroes every block_data (sky_light=torch_light=0),
	// so the NEW light gate (section 4) is unconditionally permissive here --
	// this section is purely about the burst/geometry mechanics, unchanged.
	int total = 0;
	for(int seed = 1; seed <= 300; seed++) {
		struct random_gen rng = {.seed = (uint32_t)seed};
		struct random_gen member = {.seed = (uint32_t)seed * 2654435761u};
		const struct spawner_member_ops ops
			= {.construct = NULL, .can_spawn = test_can_spawn, .emit = rec_emit};
		g_member_rng = &member;
		total += spawner_burst(&rng, 5, FLOOR + 1, 7, 0, flat_get, NULL,
							   &ops, NULL);
	}
	assert(total > 0);		   // the spawner fires on a proper floor
	assert(total == g_nrec);   // every fire was recorded
	/* I04: per member the WORLD stream draws exactly the position (4 floats,
	 * one int) and then the setLocationAndAngles yaw float
	 * [CFR TileEntityMobSpawner.java:72-75]; the light rolls are not on it. */
	{
		struct random_gen world = {.seed = 77u};
		struct random_gen want = world;
		for(int m = 0; m < SPAWNER_SPAWN_COUNT; m++) {
			for(int k = 0; k < 4; k++)
				(void)rand_gen_flt(&want);
			(void)rand_gen_int(&want, 3);
			(void)rand_gen_flt(&want);
		}
		struct random_gen member = {.seed = 99u};
		g_member_rng = &member;
		const struct spawner_member_ops ops
			= {.construct = NULL, .can_spawn = test_can_spawn, .emit = rec_emit};
		g_nrec = 0;
		(void)spawner_burst(&world, 5, FLOOR + 1, 7, 0, flat_get, NULL, &ops,
							NULL);
		assert(memcmp(&world, &want, sizeof world) == 0
			   && "world stream: position + yaw per member, no light rolls");
	}
	for(int i = 0; i < g_nrec; i++)
		assert(spawner_spot_ok(flat_get, NULL, (w_coord_t)floor(g_rec[i].x),
							   (w_coord_t)floor(g_rec[i].y),
							   (w_coord_t)floor(g_rec[i].z))); // a valid spot

	/* (5) G6 (i05-1005): the member position is the exact double
	 * xCoord + (nextDouble() - nextDouble()) * 4.0, y + nextInt(3) - 1,
	 * z likewise [CFR TileEntityMobSpawner.java:72-75] -- one stream draw per
	 * nextDouble (the server_local.c dispenser precedent: rand_gen / 2^32),
	 * handed to setLocationAndAngles as is, not floored to a block centre. */
	{
		int checked = 0, exact = 0;
		for(int seed = 1; seed <= 200; seed++) {
			struct random_gen rng = {.seed = (uint32_t)seed};
			struct random_gen model = rng;
			struct random_gen member = {.seed = (uint32_t)seed * 40503u};
			g_member_rng = &member;
			const struct spawner_member_ops ops
				= {.construct = NULL, .can_spawn = test_can_spawn, .emit = rec_emit};
			g_nrec = 0;
			(void)spawner_burst(&rng, 5, FLOOR + 1, 7, 0, flat_get, NULL, &ops,
								NULL);
			/* replay the Beta draws for member 0 and match it if emitted */
			double d1 = (double)rand_gen(&model) / 4294967296.0;
			double d2 = (double)rand_gen(&model) / 4294967296.0;
			double want_x = 5.0 + (d1 - d2) * 4.0;
			int oy = rand_gen_int(&model, 3) - 1;
			double d3 = (double)rand_gen(&model) / 4294967296.0;
			double d4 = (double)rand_gen(&model) / 4294967296.0;
			double want_z = 7.0 + (d3 - d4) * 4.0;
			if(g_nrec > 0 && oy == 0) {
				checked++;
				if(g_rec[0].x == want_x && g_rec[0].z == want_z
				   && g_rec[0].y == (double)(FLOOR + 1))
					exact++;
			}
		}
		fprintf(stderr, "  (5) member position: %d of %d first members at the "
				"exact nextDouble offset\n", exact, checked);
		assert(checked > 0);
		assert(exact == checked && "G6a: exact double offset, not a block centre");
	}

	/* (6) G6 (i05-1005): `if(var10 >= 6) { updateDelay(); return; }`
	 * [CFR TileEntityMobSpawner.java:66-70] -- per member, after
	 * createEntityInWorld and BEFORE its draws; it ENDS the burst. With the
	 * crowd full from member 1 on: member 0 draws and may spawn, member 1 is
	 * constructed then the burst returns -- no further construct, no draws. */
	{
		struct random_gen world = {.seed = 4242u};
		struct random_gen want = world;
		for(int k = 0; k < 4; k++)
			(void)rand_gen_flt(&want);
		(void)rand_gen_int(&want, 3);
		(void)rand_gen_flt(&want); /* member 0 only */
		struct random_gen member = {.seed = 7u};
		g_member_rng = &member;
		g_constructs = 0;
		g_crowd_from = 1;
		const struct spawner_member_ops ops = {.construct = count_construct,
			.too_many_nearby = crowd_full, .can_spawn = test_can_spawn,
			.emit = rec_emit};
		g_nrec = 0;
		(void)spawner_burst(&world, 5, FLOOR + 1, 7, 0, flat_get, NULL, &ops,
							NULL);
		fprintf(stderr, "  (6) crowd at member 1: %d constructs, %d emits, "
				"stream %s\n", g_constructs, g_nrec,
				memcmp(&world, &want, sizeof world) == 0 ? "= member 0 only"
														 : "MISMATCH");
		assert(g_constructs == 2 && "G6b: member 1 constructed, then the burst ends");
		assert(memcmp(&world, &want, sizeof world) == 0
			   && "G6b: no draw after the crowd check ends the burst");
		assert(g_nrec <= 1);
	}

	// (4) spawner_hostile_light_ok: Beta EntityMob.getCanSpawnHere()'s exact
	// two-roll light gate [740c583] -- see spawner.h's citation. A scripted RNG
	// pins the EXACT roll values so the short-circuit and boundary behaviour are
	// unambiguous (a real LCG only guarantees "in range", not "this value").
	{
		struct random_gen dummy = {0}; // unused when g_script_active is set

		// Open-sky suppression: raw_sky_light=15 (open sky) refuses OUTRIGHT on
		// roll 1 whenever nextInt(32) < 15 -- REGARDLESS of subtract/torch, and
		// with roll 2 never even drawn (only 1 value scripted -> an over-draw
		// would trip the "over-drawn" assert in rand_gen_int above).
		script_set(14); // 15 > 14 -> refused by roll 1 alone
		assert(!spawner_hostile_light_ok(&dummy, 15, 0, 0));
		assert(g_script_i == 1); // roll 2 was NOT drawn (short-circuit)

		// roll 1 == raw_sky_light (15 > 15 is false) -> NOT refused by roll 1,
		// falls through to roll 2. eff=15 (noon, subtract=0) always fails roll 2
		// (nextInt(8) tops out at 7, 15<=7 is never true) -- deterministically
		// refused, but via roll 2 this time (both draws consumed).
		script_set2(15, 7);
		assert(!spawner_hostile_light_ok(&dummy, 15, 0, 0));
		assert(g_script_i == 2);

		// Cave (raw_sky=0, torch=0): roll 1 can NEVER refuse (0 > anything in
		// [0,31] is false) and eff is always 0, which passes roll 2 against ANY
		// value in [0,7] -- accepted regardless of time of day or the rolls.
		script_set2(31, 0);
		assert(spawner_hostile_light_ok(&dummy, 0, 0, 11 /* midnight subtract */));
		script_set2(0, 7);
		assert(spawner_hostile_light_ok(&dummy, 0, 0, 0 /* noon subtract */));

		// Torch-lit spot (torch=14) at midnight (subtract=11): eff=max(-11,14)=14,
		// which can never pass roll 2 (max draw is 7) -- refused even at the
		// best-case roll. raw_sky_light=0 -> roll 1 always passes regardless of
		// roll32, so this exercises roll 2's ceiling.
		script_set2(0, 7);
		assert(!spawner_hostile_light_ok(&dummy, 0, 14, 11));

		// Boundary: eff==7 is a real ~12.5% chance (accepts ONLY on roll2==7),
		// NOT the old flat "always spawns" -- both outcomes must be reachable.
		// raw_sky_light=7 here, so roll32 must be >=7 (use the max, 31) or roll 1
		// would refuse it outright and never even reach roll 2.
		script_set2(31, 7);
		assert(spawner_hostile_light_ok(&dummy, 7, 0, 0)); // roll2==7 -> accept
		script_set2(31, 6);
		assert(!spawner_hostile_light_ok(&dummy, 7, 0, 0)); // roll2==6 -> refuse
		// eff==8 is a hard 0% -- no roll2 value can ever satisfy 8<=[0,7].
		// raw_sky_light=8 -> roll32 must be >=8 (31) to reach roll 2 at all.
		script_set2(31, 7);
		assert(!spawner_hostile_light_ok(&dummy, 8, 0, 0));

		// Statistical shape check: for raw_sky=15, roll 1 refuses on EXACTLY the
		// 15 values {0..14} out of 32 (~47%), never more/less -- matches the
		// dossier's "15/32 open-sky suppression" claim precisely.
		int refused_by_roll1 = 0;
		for(int r = 0; r < 32; r++) {
			script_set2(r, 7); // roll2=7 is roll-2's best case, isolating roll 1
			if(!spawner_hostile_light_ok(&dummy, 15, 0, 11))
				refused_by_roll1++;
		}
		assert(refused_by_roll1 == 15);

		g_script_active = false; // restore the LCG for any test added after this
		fprintf(stderr,
				"  (4) spawner_hostile_light_ok: two-roll gate, short-circuit, "
				"boundary + open-sky suppression all PASS\n");
	}

	printf("test_spawner: spot-check + mob mapping + %d burst spawns on a flat "
		   "floor (all valid) + light-gate two-roll shape\n",
		   total);
	return 0;
}
