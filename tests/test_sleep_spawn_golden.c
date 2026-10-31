/* next-gaps-0924/P7 -- Beta 1.7.3 sleep spawning golden (docs/intent/p7-sleep-spawns.md).
 *
 * The spec (decompile 740c583, net/minecraft/src):
 *   World.tick:1740-1752         all players fully asleep -> if(spawnHostileMobs && difficultySetting >= 1)
 *                                SpawnerAnimals.performSleepSpawning; ONLY when it returns false: skip to dawn +
 *                                wakeUpAllPlayers
 *   SpawnerAnimals.java:169-246  performSleepSpawning (20 candidates per sleeper, nightSpawnEntities :10,
 *                                canCreatureTypeSpawnAtLocation :153, getCanSpawnHere, Pathfinder range 32,
 *                                path end within 1.5, BlockBed.getNearestEmptyChunkCoordinates(..., 1) or the
 *                                candidate (x, y+1, z), creatureSpecificInit, wakeUpPlayer(true,false,false))
 *   BlockBed.java:158-182        getNearestEmptyChunkCoordinates: 3x3 around the head, then around the foot
 *                                (foot = head - headBlockToFootBlockMap[dir]); x outer, z inner; floor
 *                                isBlockNormalCube, cell + above isAirBlock; skip the first `var4` hits
 *   EntityPlayer.wakeUpPlayer:632-646  reposition via the same method with skip 0; immediately -> timer 0
 *
 * Fixture: a superflat void world with a stone field, a bed at (0,Y,0) head / (0,Y,-1) foot, midnight.
 * The expected wake/placement cells below are computed by hand from BlockBed's loop, not from CaveWii.
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
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
#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/entity_monster.h"
#include "entity/pathfinding.h"
#include "entity/monsters_object.h"
#include "game/difficulty.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "network/spawner.h"
#include "platform/thread.h"
#include "worldgen/java_random.h"

void server_local_test_set_difficulty(int d);

#define Y 70
#define FIELD 40
static char fixture[128];
static struct server_local s;
static volatile int draining = 1;
static int failures = 0;

static void* drain_entry(void* arg) {
	(void)arg;
	while(draining) { clin_drain_discard(); thread_msleep(1); }
	clin_drain_discard();
	return NULL;
}

static void set(int x, int y, int z, int type, int meta) {
	if(server_world_set_block(&s.worlds[0], x, y, z,
		(struct block_data) {.type = type, .metadata = meta})) return;
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	assert(b.type == type);
}

static void check(bool ok, const char* what) {
	if(!ok) {
		failures++;
		fprintf(stderr, "RED %s\n", what);
	}
}

static void clear_entities(void) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(e) free(e);
	}
	dict_entity_reset(s.entities);
}

static int count_monsters(int* last_id, float pos[3]) {
	int n = 0;
	dict_entity_it_t it;
	for(dict_entity_it(it, s.entities); !dict_entity_end_p(it); dict_entity_next(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_MONSTER) {
			n++;
			if(last_id) *last_id = e->data.monster.id;
			if(pos) { pos[0] = e->pos[0]; pos[1] = e->pos[1]; pos[2] = e->pos[2]; }
		}
	}
	return n;
}

/* Put the host to bed at the fixture bed, one tick short of fully asleep. */
static void prepare_sleep(uint32_t seed) {
	clear_entities();
	s.world_time = 18000;
	s.player.has_bed_spawn = false;
	server_local_begin_sleep(&s, 0, Y, 0);
	assert(s.player.sleeping);
	s.player.sleep_timer = 99;
	s.rand_src.seed = seed;
	s.rand_src.has_next_gaussian = false;
}

/* ... and run the tick that completes the sleep (timer 99 -> 100). */
static void sleep_until_full(uint32_t seed) {
	prepare_sleep(seed);
	server_local_sleep_tick(&s);
}

/* ---- independent transcription of SpawnerAnimals.performSleepSpawning (SpawnerAnimals.java:169-246) --- */

static struct block_data ref_block(int x, int y, int z) { // World.getBlockId: EmptyChunk / out of range = 0
	struct block_data b = {.type = BLOCK_AIR};
	if(y >= 0 && y < 128 && !server_world_get_block(&s.worlds[0], x, y, z, &b))
		b = (struct block_data) {.type = BLOCK_AIR};
	return b;
}
static bool ref_normal(int x, int y, int z) { return block_is_normal_cube(ref_block(x, y, z).type); }
static bool ref_air(int x, int y, int z) { return ref_block(x, y, z).type == BLOCK_AIR; }
static bool ref_liquid(int x, int y, int z) {
	int id = ref_block(x, y, z).type;
	return id == BLOCK_WATER_FLOW || id == BLOCK_WATER_STILL || id == BLOCK_LAVA_FLOW || id == BLOCK_LAVA_STILL;
}
// canCreatureTypeSpawnAtLocation(monster), SpawnerAnimals.java:153-155.
static bool ref_can_spawn(int x, int y, int z) {
	return ref_normal(x, y - 1, z) && !ref_normal(x, y, z) && !ref_liquid(x, y, z) && !ref_normal(x, y + 1, z);
}
// BlockBed.getNearestEmptyChunkCoordinates for the fixture bed: head (0,Y,0), headBlockToFootBlockMap = (0,1).
static bool ref_bed_cell(int skip, int* cx, int* cy, int* cz) {
	const int x = 0, y = Y, z = 0, mx = 0, mz = 1;
	for(int v7 = 0; v7 <= 1; v7++) {
		int v8 = x - mx * v7 - 1, v9 = z - mz * v7 - 1;
		for(int v12 = v8; v12 <= v8 + 2; v12++)
			for(int v13 = v9; v13 <= v9 + 2; v13++)
				if(ref_normal(v12, y - 1, v13) && ref_air(v12, y, v13) && ref_air(v12, y + 1, v13)) {
					if(skip <= 0) { *cx = v12; *cy = y; *cz = v13; return true; }
					skip--;
				}
	}
	return false;
}

/* RNG owners (R-B02, 2026-10-09). World.rand (s.rand_src) draws the candidate's x/z/y offsets, the mob pick
 * and setLocationAndAngles' yaw (SpawnerAnimals.java:191-196, :221) plus creatureSpecificInit's spider-jockey
 * nextInt(100) (:158, World.rand). EntityMob.getCanSpawnHere's two light rolls are on the CANDIDATE's own
 * rand (EntityMob.java:73, :84; Entity.java:50, one Random per entity), a fresh entity
 * built by newInstance (:216) that takes Entity.nextEntityID (consumed even when refused). The engine's
 * sanctioned lifetime adaptation of that Random is entity_lifetime_rng_seed(world seed, dim, id) (Java48 mob-RNG
 * slice J/N, 0db9e79ea); the spawned mob CONTINUES that stream. Beta joins the mob with zero motion (Entity.java:
 * 20-22; newborn-impulse removal, slice I, 0db9e79ea), so no velocity draws. The jockey skeleton is built with
 * the spider's yaw (:160) -- no world draw. The closing playLivingSound() on the new mob (:238) draws its pitch,
 * (rand.nextFloat() - rand.nextFloat()) * 0.2F + 1.0F, on the NEW mob's own Random
 * (EntityLiving.java:96-100): spider/zombie/skeleton all have a non-null living sound, so the spawned stream
 * is the candidate stream plus two nextFloat draws (LIVING-SOUND-RNG). */
static uint32_t ref_gen_id(uint32_t* next) { // server_local_gen_id on a cleared dictionary
	do { ++*next; } while(*next == 0 || *next == UINT32_MAX);
	return *next;
}

struct ref_result {
	bool spawned;
	int mob, cx, cy, cz, attempt, candidates_scanned_ok;
	uint32_t id;               // the spawned monster's id (the last candidate constructed)
	uint32_t next_id;          // the id counter after the leg (every constructed candidate, + a jockey skeleton)
	struct java_random mob_rng; // the spawned monster's own Random after getCanSpawnHere
	struct random_gen rng;
	struct random_gen rng_before_spawn; // w4: world state before creatureSpecificInit's jockey draw
};

static struct ref_result reference(uint32_t seed) {
	static const int night[3] = {MONSTER_SPIDER, MONSTER_ZOMBIE, MONSTER_SKELETON}; // SpawnerAnimals.java:10
	struct ref_result r = {0};
	r.rng = (struct random_gen) {.seed = seed};
	r.next_id = s.next_entity_id;
	struct random_gen* g = &r.rng;
	double px = s.player.x, py = s.player.y, pz = s.player.z;
	for(int v8 = 0; v8 < 20; v8++) {
		int a = rand_gen_int(g, 32), b = rand_gen_int(g, 32);
		int v9 = (int)floor(px) + a - b;
		a = rand_gen_int(g, 32); b = rand_gen_int(g, 32);
		int v10 = (int)floor(pz) + a - b;
		a = rand_gen_int(g, 16); b = rand_gen_int(g, 16);
		int v11 = (int)floor(py) + a - b;
		if(v11 < 1) v11 = 1; else if(v11 > 128) v11 = 128;
		int v12 = rand_gen_int(g, 3);
		int v13;
		for(v13 = v11; v13 > 2 && !ref_normal(v9, v13 - 1, v10); --v13) {}
		while(!ref_can_spawn(v9, v13, v10) && v13 < v11 + 16 && v13 < 128) ++v13;
		if(!(v13 < v11 + 16 && v13 < 128)) continue;
		r.candidates_scanned_ok++;
		int mob = night[v12];
		uint32_t id = ref_gen_id(&r.next_id); // newInstance: Entity.nextEntityID++ and the entity's own Random
		struct java_random own;
		entity_lifetime_rng_seed(&own, s.worlds[0].seed, s.worlds[0].dimension, id);
		(void)rand_gen_flt(g); // setLocationAndAngles yaw on World.rand (:221)
		struct block_data here = ref_block(v9, v13, v10);
		if((int)here.sky_light > jrand_next_int_bound(&own, 32)) continue;       // EntityMob.java:73
		if(server_local_hostile_spawn_light(&s, v9, v13, v10) > jrand_next_int_bound(&own, 8)) continue; // :84
		if(!server_local_test_mob_spawn_bbox_clear(&s, mob, v9, v13, v10)) continue;
		const struct monster_def* d = monster_def_get(mob);
		struct path_entity path;
		if(!monster_path_to_entity(&s, (float)v9 + 0.5F, (float)v13, (float)v10 + 0.5F, d->box_w, d->box_h,
			   (float)px, (float)(py - 0.2), (float)pz, 32.0F, &path) || path.length <= 1)
			continue;
		const struct path_waypoint* e = &path.points[path.length - 1];
		if(!(fabs(e->x - px) < 1.5 && fabs(e->z - pz) < 1.5 && fabs(e->y - py) < 1.5)) continue;
		if(!ref_bed_cell(1, &r.cx, &r.cy, &r.cz)) { r.cx = v9; r.cy = v13 + 1; r.cz = v10; }
		r.spawned = true;
		r.mob = mob;
		r.attempt = v8;
		r.id = id;
		r.rng_before_spawn = r.rng;
		// creatureSpecificInit (:158): the spider jockey's world nextInt(100); a hit builds a skeleton (next id).
		if(mob == MONSTER_SPIDER && rand_gen_int(g, 100) == 0) (void)ref_gen_id(&r.next_id);
		// playLivingSound (:238) on the spawned mob's own Random: two pitch floats.
		(void)jrand_next_float(&own);
		(void)jrand_next_float(&own);
		r.mob_rng = own;
		return r;
	}
	return r;
}

/* The world time a completed sleep skip leaves behind. server_local_sleep_tick runs AFTER the tick's
 * `world_time++`, so the Beta tick-start time is T = W - 1 for a sleep tick entered at W. World.tick:1748-1751
 * sets time = (T + 24000) - (T + 24000) % 24000 (the next dawn D strictly after T), and World.tick:1765-1770
 * then advances the clock once more: the tick ends at D + 1. */
static uint64_t beta_skip_end(uint64_t w) {
	uint64_t t = w - 1;
	uint64_t v = t + 24000;
	return v - v % 24000 + 1;
}

/* ---- goldens -------------------------------------------------------------------------------------- */

// Open lit field at midnight: Beta's per-sleep spawn probability is high (every candidate column has a
// stone floor reachable by a range-32 path; sky 15 - subtract 11 = 4 passes both light rolls often).
static void case_spawn_happens(void) {
	int spawned_seeds = 0, first = 0;
	for(uint32_t seed = 1; seed <= 40; seed++) {
		sleep_until_full(seed * 7919u);
		int mob = -1;
		float pos[3] = {0};
		int n = count_monsters(&mob, pos);
		if(n > 0) {
			if(!spawned_seeds) first = (int)seed;
			spawned_seeds++;
			// Beta: the spawn wakes the sleeper IMMEDIATELY (timer 0), cancels the skip, sets no spawn point.
			check(!s.player.sleeping && s.player.sleep_timer == 0, "spawn wakes the sleeper immediately (timer 0)");
			check(s.world_time == 18000, "spawn cancels the skip to dawn");
			check(!s.player.has_bed_spawn, "wakeUpPlayer(true,false,false) sets no spawn point");
			check(mob == MONSTER_SPIDER || mob == MONSTER_ZOMBIE || mob == MONSTER_SKELETON,
				"nightSpawnEntities = spider / zombie / skeleton");
		} else {
			check(s.world_time == beta_skip_end(18000) && !s.player.sleeping, "no spawn: skip to dawn + wake as before");
		}
	}
	printf("open field, midnight, 40 seeds: %d spawned a monster (first seed index %d)\n", spawned_seeds, first);
	check(spawned_seeds > 0, "Beta performSleepSpawning spawns next to an open, reachable field");
}

// Peaceful: World.tick's difficultySetting >= 1 gate -- no spawn, no draw, the skip happens.
static void case_peaceful(void) {
	server_local_test_set_difficulty(DIFFICULTY_PEACEFUL);
	sleep_until_full(12345u);
	struct random_gen untouched = {.seed = 12345u};
	check(count_monsters(NULL, NULL) == 0, "Peaceful: no sleep spawn");
	check(s.rand_src.seed == untouched.seed, "Peaceful: performSleepSpawning never runs (no draw)");
	check(s.world_time == beta_skip_end(18000) && !s.player.sleeping, "Peaceful: skip to dawn");
	server_local_test_set_difficulty(DIFFICULTY_NORMAL);
}

// Wake reposition (skip 0): 3x3 around the head (0,Y,0), x outer from -1, z inner from -1 -> the FIRST
// valid cell is (-1,Y,-1) (floor stone, air, air). The old 4-cardinal scan picked (+1,Y,0).
static void case_wake_cell(void) {
	server_local_test_set_difficulty(DIFFICULTY_PEACEFUL); // plain skip + wakeUpAllPlayers
	sleep_until_full(1u);
	printf("wake after skip: player at (%.3f, %.4f, %.3f)\n", s.player.x, s.player.y, s.player.z);
	check(s.player.x == -0.5 && s.player.z == -0.5, "wake reposition = first BlockBed 3x3 cell (-1,Y,-1)");
	check(s.player.y == (double)Y + 0.1 + PLAYER_EYE_HEIGHT, "wake eye = cell y + yOffset + 0.1");
	server_local_test_set_difficulty(DIFFICULTY_NORMAL);
}


// Production == the Java transcription, seed by seed: spawn or not, type, placement cell, every draw.
static void case_reference_equiv(void) {
	int spawned = 0, mism = 0;
	double worst_ms = 0.0;
	int32_t worst_searches = 0, worst_nodes = 0;
	for(uint32_t seed = 1; seed <= 60; seed++) {
		prepare_sleep(seed * 104729u);
		struct ref_result r = reference(seed * 104729u);
		pathfinding_perf_tick_reset();
		clock_t c0 = clock();
		server_local_sleep_tick(&s);
		double ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
		if(ms > worst_ms) worst_ms = ms;
		pathfinding_perf_tick_reset(); // latch this tick's search count
		int32_t searches = 0, nodes_last = 0, nodes_max = 0;
		pathfinding_perf_counters(&searches, &nodes_last, &nodes_max);
		if(searches > worst_searches) worst_searches = searches;
		if(nodes_max > worst_nodes) worst_nodes = nodes_max;
		int mob = -1;
		float pos[3] = {0};
		int n = count_monsters(&mob, pos);
		bool ok = (n > 0) == r.spawned && s.rand_src.seed == r.rng.seed && s.next_entity_id == r.next_id;
		if(getenv("CW_W3_DUMP")) // w3 measurement: per-seed outcome, diffed across pool-capacity builds
			printf("W3 seed=%u spawned=%d mob=%d cell=%d,%d,%d attempt=%d rng=%u\n", seed, r.spawned, r.mob,
				r.cx, r.cy, r.cz, r.attempt, (unsigned)r.rng.seed);
		if(r.spawned) {
			spawned++;
			bool found = false;
			dict_entity_it_t it;
			for(dict_entity_it(it, s.entities); !dict_entity_end_p(it); dict_entity_next(it)) {
				struct entity* e = dict_entity_ref(it)->value;
				if(e && e->type == ENTITY_MONSTER && e->data.monster.id == r.mob && e->id == r.id
				   && e->gameplay_rng.seed == r.mob_rng.seed
				   && e->pos[0] == (float)r.cx + 0.5F && e->pos[1] == (float)r.cy
				   && e->pos[2] == (float)r.cz + 0.5F)
					found = true;
			}
			ok = ok && found && !s.player.sleeping && s.world_time == 18000;
		} else {
			ok = ok && s.world_time == beta_skip_end(18000);
		}
		if(!ok) {
			if(mism < 6)
				fprintf(stderr, "  seed %u: ref spawned=%d mob=%d cell=(%d,%d,%d) | prod monsters=%d mob=%d "
					"pos=(%.2f,%.2f,%.2f) rng %s\n", seed, r.spawned, r.mob, r.cx, r.cy, r.cz, n, mob,
					pos[0], pos[1], pos[2], s.rand_src.seed == r.rng.seed ? "same" : "DIFFERS");
			mism++;
		}
	}
	printf("reference transcription vs production, 60 seeds: %d spawns, mismatches=%d\n", spawned, mism);
	printf("cost of the completing sleep tick (PC, informational -- the Wii number comes from the rig): "
		"worst %.2f ms, worst %d A* searches in one tick, peak %d nodes in one search (pool cap 1024)\n",
		worst_ms, (int)worst_searches, (int)worst_nodes);
	check(mism == 0 && spawned > 0, "production performSleepSpawning == the Java transcription (result + draws)");
}

// Placement golden (by hand from BlockBed.java:158-182 on the fixture): the monster takes the SECOND
// empty cell of the head's 3x3, (-1,Y,0); the woken sleeper the FIRST, (-1,Y,-1). Never the same cell.
static void case_placement(void) {
	for(uint32_t seed = 1; seed <= 200; seed++) {
		prepare_sleep(seed);
		struct ref_result r = reference(seed);
		if(!r.spawned) continue;
		server_local_sleep_tick(&s);
		float pos[3] = {0};
		(void)count_monsters(NULL, pos);
		printf("placement (seed %u): monster at (%.2f, %.2f, %.2f), woken sleeper at (%.2f, %.4f, %.2f)\n",
			seed, pos[0], pos[1], pos[2], s.player.x, s.player.y, s.player.z);
		check(pos[0] == -0.5F && pos[1] == (float)Y && pos[2] == 0.5F,
			"monster placed on BlockBed empty cell #2 (skip 1) = (-1,Y,0)");
		check(s.player.x == -0.5 && s.player.z == -0.5, "woken sleeper on empty cell #1 (skip 0) = (-1,Y,-1)");
		return;
	}
	check(false, "no spawning seed found for the placement golden");
}

// TEMPORARY carve-out (docs/deviations.md, GUEST_NO_SLEEP_SKIP_2026_09_24): guest sleeping is fail-closed, so an
// admitted guest is NOT counted by isAllPlayersFullyAsleep -- the sleep spawn and the skip behave exactly as
// with the host alone (same draws, same outcome). Flip when guest sleeping lands (Beta: an awake guest blocks).
/* A1d (split-dimensions-plan-0924 row #3): the P7b carve-out is REVERTED --
 * guest sleeping is live, so an admitted AWAKE guest in the Overworld keeps the
 * night exactly as Beta requires: World.isAllPlayersFullyAsleep
 * (World.java:2379-2394) is false with one awake player, so World.tick
 * (:1740-1752) runs neither performSleepSpawning nor the skip. Pinned per seed:
 * no draw from the world RNG, no monster, the clock unchanged, and the host
 * still asleep with its timer saturated at 100. */
static void case_guest_admitted(void) {
	struct server_player guest;
	memset(&guest, 0, sizeof guest); // awake, Overworld (dimension 0)
	int agree = 0;
	for(uint32_t seed = 1; seed <= 20; seed++) {
		prepare_sleep(seed * 31337u);
		s.guest_gameplay = true;
		s.guest_actor = &guest;
		server_local_sleep_tick(&s);
		s.guest_gameplay = false;
		s.guest_actor = NULL;
		bool ok = s.rand_src.seed == seed * 31337u && count_monsters(NULL, NULL) == 0
			&& s.world_time == 18000 && s.player.sleeping && s.player.sleep_timer == 100;
		agree += ok;
	}
	printf("admitted awake guest (Beta quorum), 20 seeds: %d keep the night (no draw, no spawn, no skip)\n",
		agree);
	check(agree == 20,
		"an admitted AWAKE guest blocks the skip and the sleep spawn (World.isAllPlayersFullyAsleep)");
}

// Before the stone field exists the only floor is the superflat ground (spawn height 5, ~65 blocks under
// the bed): candidates DO qualify there and draw their yaw/light rolls, but no range-32 path can end within
// 1.5 of the sleeper -> nothing spawns, every draw matches the transcription, then the skip; the wake falls
// back atop the bed head (no 3x3 cell has a normal-cube floor + two air cells).
static void case_no_spawn_void(void) {
	prepare_sleep(4242u);
	struct ref_result r = reference(4242u);
	server_local_sleep_tick(&s);
	printf("void fixture: ref spawned=%d candidates=%d; skip to %llu; wake at (%.2f, %.4f, %.2f)\n", r.spawned,
		r.candidates_scanned_ok, (unsigned long long)s.world_time, s.player.x, s.player.y, s.player.z);
	check(!r.spawned && count_monsters(NULL, NULL) == 0, "void: nothing spawns");
	check(r.candidates_scanned_ok > 0, "void: candidates reach the far ground (exercises the path gate)");
	check(s.rand_src.seed == r.rng.seed, "void: draws == the transcription");
	check(s.next_entity_id == r.next_id, "void: one candidate id per qualifying candidate (newInstance)");
	check(s.world_time == beta_skip_end(18000) && !s.player.sleeping, "void: skip to dawn");
	check(s.player.x == 0.5 && s.player.z == 0.5 && s.player.y == (double)Y + 1.0 + 0.1 + PLAYER_EYE_HEIGHT,
		"void: wake fallback atop the bed head (x, y+1, z)");
}

// w1: the skip's end tick (World.tick:1748-1751 + :1765-1770). A sleep completing at tick-start T lands on the
// next dawn D after T and the same tick then advances to D + 1. Landmarks by hand from the Java:
//   W=18000 (T=17999) -> D=24000 -> 24001;  W=23459, the last tick before isDaytime (subtract < 4 from
//   t=23460) -> D=24000 -> 24001. The "T+1 is a multiple of 24000" corner (where next-dawn-after-W and
//   next-dawn-after-T disagree by a whole day) is unreachable: by then isDaytime has already woken the
//   sleeper with no skip -- pinned at W=23460 below.
static void case_skip_end_tick(void) {
	const uint64_t ws[2] = {18000, 23459};
	server_local_test_set_difficulty(DIFFICULTY_PEACEFUL);
	for(int i = 0; i < 2; i++) {
		assert(beta_skip_end(ws[i]) == 24001);
		prepare_sleep(1u);
		s.world_time = ws[i];
		server_local_sleep_tick(&s);
		printf("skip end: sleep tick entered at W=%llu -> world_time %llu (Beta 24001)\n",
			(unsigned long long)ws[i], (unsigned long long)s.world_time);
		check(!s.player.sleeping && s.world_time == 24001, "skip ends at Beta's dawn D + 1");
	}
	prepare_sleep(1u);
	s.world_time = 23460;
	server_local_sleep_tick(&s);
	check(!s.player.sleeping && s.world_time == 23460, "W=23460 is daytime: woken by daylight, no skip");
	server_local_test_set_difficulty(DIFFICULTY_NORMAL);
}

// w4: Beta's entityJoinedWorld cannot fail; this engine's spawn can (entity pool / dictionary). A failed
// sleep-spawn allocation is "no spawn": no wake from the spawn path, the skip to dawn proceeds, and no
// spawn / jockey draws are made after the failure. Injected with the existing alloc-failure hook.
void server_local_test_fail_next_entity_alloc(void);
static void case_spawn_alloc_failure(void) {
	for(uint32_t seed = 1; seed <= 200; seed++) {
		prepare_sleep(seed);
		struct ref_result r = reference(seed);
		if(!r.spawned) continue;
		server_local_test_fail_next_entity_alloc();
		server_local_sleep_tick(&s);
		printf("injected spawn failure (seed %u): monsters=%d, clock %llu, sleeping=%d, spawn point=%d\n", seed,
			count_monsters(NULL, NULL), (unsigned long long)s.world_time, s.player.sleeping, s.player.has_bed_spawn);
		check(count_monsters(NULL, NULL) == 0, "failed spawn: no monster");
		check(s.world_time == beta_skip_end(18000) && !s.player.sleeping && s.player.has_bed_spawn,
			"failed spawn = no spawn: the skip to dawn and wakeUpAllPlayers(setSpawn) proceed");
		check(s.rand_src.seed == r.rng_before_spawn.seed, "failed spawn: no spawn/jockey draws after the failure");
		check(s.next_entity_id == r.id, "failed spawn: no id taken after the refused allocation");
		return;
	}
	check(false, "no spawning seed for the alloc-failure golden");
}

int main(void) {
	test_fixture_dir("test_sleep_spawn_golden_tmp", fixture, sizeof fixture);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	MKDIR(fixture);
	MKDIR(test_fixture_join(fixture, "/region"));
	remove(test_fixture_join(fixture, "/region/r.0.0.mcr"));
	remove(test_fixture_join(fixture, "/region/r.-1.0.mcr"));
	remove(test_fixture_join(fixture, "/region/r.0.-1.mcr"));
	remove(test_fixture_join(fixture, "/region/r.-1.-1.mcr"));
	string_t level;
	string_init_set_str(level, fixture);
	memset(&s, 0, sizeof s);
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s, &s.player,
		INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	for(int cx = -4; cx <= 3; cx++)
		for(int cz = -4; cz <= 3; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	// Bed: foot (0,Y,-1) meta 0, head (0,Y,0) meta 8 (head bit, dir index 0 = +Z), on two stone supports.
	set(0, Y - 1, -1, BLOCK_STONE, 0);
	set(0, Y - 1, 0, BLOCK_STONE, 0);
	set(0, Y, -1, BLOCK_BED, 0);
	set(0, Y, 0, BLOCK_BED, 8);
	server_local_test_set_difficulty(DIFFICULTY_NORMAL);
	case_no_spawn_void();

	for(int x = -FIELD; x <= FIELD; x++)
		for(int z = -FIELD; z <= FIELD; z++)
			set(x, Y - 1, z, BLOCK_STONE, 0);

	case_skip_end_tick();
	case_peaceful();
	case_wake_cell();
	case_guest_admitted();
	case_spawn_happens();
	case_reference_equiv();
	case_placement();
	case_spawn_alloc_failure();

	clear_entities();
	draining = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	test_fixture_dir_cleanup(fixture);
	if(failures) {
		fprintf(stderr, "test_sleep_spawn_golden: %d FAILED\n", failures);
		return 1;
	}
	printf("test_sleep_spawn_golden: all PASS\n");
	return 0;
}
