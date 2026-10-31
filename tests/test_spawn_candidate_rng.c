/* Copyright (c) 2026 CaveWii_v2 contributors. GPL-3.0-or-later.
 *
 * test_spawn_candidate_rng -- I03 (board #8): natural-spawn eligibility rolls
 * are drawn from the CANDIDATE's own Random, constructed in Beta's order.
 *
 * Beta (CFR server decompile, net/minecraft/src):
 *   SpawnerAnimals.java:123  cell + no player within 24 + 576 from spawn, else
 *                            `continue` BEFORE any construction;
 *   SpawnerAnimals.java:125  newInstance(world) -- Entity takes
 *                            nextEntityID++ (Entity.java:54) and its own
 *                            Random (Entity.java:96), even if later refused;
 *   EntitySlime.java:38-40   constructor: size 1 << rand.nextInt(3), then
 *                            ticksTillJump = rand.nextInt(20) + 10;
 *   SpawnerAnimals.java:132  getCanSpawnHere() on that instance:
 *     EntityMob.java:92-102    sky > rand.nextInt(32) -> no; light <= rand.nextInt(8)
 *     EntityGhast.java:171     rand.nextInt(20) == 0 && super...
 *     EntitySlime.java:154     rand.nextInt(10) == 0 && chunk nextInt(10) && y < 16
 *     EntityPigZombie          no draw.
 * The lifetime stream here is entity_lifetime_rng_seed(world seed, dim, id).
 *
 * Checks over many real spawn passes (real Overworld and Nether terrain):
 *   r1  every spawned mob's own stream sits exactly after Beta's constructor
 *       + getCanSpawnHere draws for its kind (seeded from ITS id);
 *   r2  refused candidates consumed ids (ids used > mobs spawned), as Beta.
 * Spider-jockey riders are excluded (constructed by the jockey roll).
 */
#include <assert.h>
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
#include "entity/monsters_object.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "network/server_spawner.h"
#include "platform/thread.h"
#include "worldgen/java_random.h"
#include "world.h"

static char s_dir[160];
static struct server_local s;
static volatile int g_drain_run = 1;
static int failures;

static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void check(bool ok, const char* what) {
	printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
	if(!ok)
		failures++;
}

/* The stream position Beta reaches for a refused-or-accepted candidate of
 * this kind that was ACCEPTED (all its getCanSpawnHere draws happened). */
static bool expected_state(const struct entity* e, struct java_random* out) {
	const struct server_world* w = &s.worlds[0];
	entity_lifetime_rng_seed(out, w->seed, w->dimension, e->id);
	switch(e->data.monster.id) {
		case MONSTER_SLIME_SMALL:
		case MONSTER_SLIME_MEDIUM:
		case MONSTER_SLIME_BIG: {
			int size_roll = jrand_next_int_bound(out, 3);
			if(slime_size_from_roll(size_roll) != e->data.monster.id)
				return false; /* the size must come from its own stream */
			(void)jrand_next_int_bound(out, 20); /* ticksTillJump */
			(void)jrand_next_int_bound(out, 10); /* getCanSpawnHere */
			return true;
		}
		case MONSTER_GHAST:
			(void)jrand_next_int_bound(out, 20);
			return true;
		case MONSTER_PIG_ZOMBIE:
			return true;
		default:
			(void)jrand_next_int_bound(out, 32);
			(void)jrand_next_int_bound(out, 8);
			return true;
	}
}

static void clear_entities(void) {
	dict_entity_it_t it;
	dict_entity_it(it, s.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* e = dict_entity_ref(it)->value;
		dict_entity_next(it);
		if(e)
			free(e);
	}
	dict_entity_reset(s.entities);
}

/* s1-s3: the slime candidate (no natural slime spawned in the fixtures --
 * slime chunks below y 16 are rare), driven through the production pair. */
static void slime_candidate_golden(void) {
	const struct random_gen world_before = s.rand_src;
	const uint32_t next_before = s.next_entity_id;
	int accepted = 0, sizes[3] = {0}, stream_ok = 0;
	for(int i = 0; i < 200; i++) {
		uint32_t id;
		int mob;
		struct java_random got;
		bool ok = server_local_test_spawn_candidate(&s, &s.worlds[0],
			MONSTER_SLIME_SMALL, 8, 10, 8, &id, &mob, &got);
		accepted += ok;
		struct java_random want;
		entity_lifetime_rng_seed(&want, s.worlds[0].seed,
								 s.worlds[0].dimension, id);
		int roll = jrand_next_int_bound(&want, 3);
		(void)jrand_next_int_bound(&want, 20);
		(void)jrand_next_int_bound(&want, 10);
		if(slime_size_from_roll(roll) == mob && want.seed == got.seed)
			stream_ok++;
		if(mob >= MONSTER_SLIME_SMALL && mob <= MONSTER_SLIME_BIG)
			sizes[mob - MONSTER_SLIME_SMALL]++;
	}
	printf("  slime candidates: 200 built, %d accepted, sizes %d/%d/%d, %d "
		   "on Beta's own-stream position\n", accepted, sizes[0], sizes[1],
		   sizes[2], stream_ok);
	check(stream_ok == 200, "s1: slime size, ticksTillJump and the nextInt(10) "
		  "roll are the candidate's own draws (EntitySlime.java:38-40, :154)");
	check(memcmp(&world_before, &s.rand_src, sizeof world_before) == 0,
		  "s2: building and rolling slime candidates draws nothing from the "
		  "world stream");
	check(s.next_entity_id - next_before == 200,
		  "s3: every candidate takes one id, accepted or not (Entity.java:54)");
}

/* c1-c3: chest-cart break spill on the CART's own Random [CFR
 * EntityMinecart.java:119-135]: 3 offset floats per slot, chunks of
 * nextInt(21)+10, 3 gaussians per chunk; the world stream untouched. */
static void cart_spill_golden(void) {
	struct entity* cart = server_local_spawn_minecart_type(
		(vec3) {8.5F, 70.0F, 8.5F}, &s, 1);
	assert(cart && cart->data.minecart.minecart_type == 1);
	memset(cart->data.minecart.inventory, 0,
		   sizeof cart->data.minecart.inventory);
	cart->data.minecart.inventory[0]
		= (struct item_data) {.id = BLOCK_COBBLESTONE, .count = 64};
	cart->data.minecart.inventory[5]
		= (struct item_data) {.id = ITEM_COAL, .count = 7};
	struct java_random want = cart->gameplay_rng;
	struct java_random fresh;
	entity_lifetime_rng_seed(&fresh, s.worlds[0].seed, s.worlds[0].dimension,
							 cart->id);
	const bool seeded = fresh.seed == want.seed;
	int chunks = 0;
	const int stacks[2] = {64, 7};
	for(int k = 0; k < 2; k++) {
		for(int i = 0; i < 3; i++)
			(void)jrand_next_float(&want);
		int left = stacks[k];
		while(left > 0) {
			int n = jrand_next_int_bound(&want, 21) + 10;
			left -= n < left ? n : left;
			for(int i = 0; i < 3; i++)
				(void)jrand_next_gaussian(&want);
			chunks++;
		}
	}
	const struct random_gen world_before = s.rand_src;
	const uint32_t next_before = s.next_entity_id;
	server_local_minecart_break_drops(&s, cart);
	/* items spawned: chunks + the cart item + the chest block */
	const uint32_t spawned = s.next_entity_id - next_before;
	printf("  chest cart spill: %d expected chunks, %u entities spawned\n",
		   chunks, (unsigned)spawned);
	check(seeded, "c0: the cart is born with its lifetime stream "
				  "(Entity.java:96)");
	check(cart->gameplay_rng.seed == want.seed
			  && spawned == (uint32_t)chunks + 2u,
		  "c1: the spill is 3 offsets/slot, nextInt(21)+10 chunks and 3 "
		  "gaussians/chunk on the cart's own Random (EntityMinecart.java:"
		  "119-135)");
	check(memcmp(&world_before, &s.rand_src, sizeof world_before) == 0,
		  "c2: the spill draws nothing from the world stream");
}

int server_local_test_tick_spawners(struct server_local* s);

/* d1-d2 (I04): dungeon spawner members [CFR TileEntityMobSpawner.java:
 * 62-76]: createEntityInWorld (an id + its own Random) per member, then the
 * world-stream position and yaw, then getCanSpawnHere on the member's own
 * Random -- EntityMob's nextInt(32) / nextInt(8) (EntityMob.java:92-102). */
static void dungeon_spawner_golden(void) {
	const int sx = 8, sy = 20, sz = 8;
	for(int x = sx - 6; x <= sx + 6; x++)
		for(int z = sz - 6; z <= sz + 6; z++) {
			for(int y = sy - 1; y <= sy + 3; y++)
				server_world_set_block_silent(&s.worlds[0], x, y, z,
					(struct block_data) {.type = (y == sy - 1 || y == sy + 3)
						? BLOCK_STONE : BLOCK_AIR});
		}
	server_world_set_block_silent(&s.worlds[0], sx, sy, sz,
		(struct block_data) {.type = BLOCK_MOB_SPAWNER});
	s.player.x = sx + 0.5;
	s.player.y = sy + 1.62;
	s.player.z = sz + 3.5;
	clear_entities();
	int spawned = 0, matched = 0, bursts = 0;
	const uint32_t ids_before = s.next_entity_id;
	for(int t = 0; t < 400 && spawned < 12; t++) {
		uint32_t before = s.next_entity_id;
		server_local_test_spawner_fire(&s, sx, sy, sz, MONSTER_ZOMBIE);
		bursts++;
		for(uint32_t id = before; id < s.next_entity_id; id++) {
			struct entity** pp = dict_entity_get(s.entities, id);
			if(!pp || !*pp || (*pp)->type != ENTITY_MONSTER)
				continue;
			spawned++;
			struct java_random want;
			entity_lifetime_rng_seed(&want, s.worlds[0].seed,
									 s.worlds[0].dimension, id);
			(void)jrand_next_int_bound(&want, 32);
			(void)jrand_next_int_bound(&want, 8);
			if(want.seed == (*pp)->gameplay_rng.seed)
				matched++;
		}
		clear_entities(); /* keep the crowd count (>= 6 ends a burst) open */
	}
	const uint32_t ids_used = s.next_entity_id - ids_before;
	printf("  dungeon spawner: %d zombies, %d on Beta's own-stream position, "
		   "%u ids used\n", spawned, matched, (unsigned)ids_used);
	assert(spawned > 0 && "fixture: the dungeon spawner fired");
	check(matched == spawned,
		  "d1: each spawner member's light rolls are its own nextInt(32)/(8) "
		  "(TileEntityMobSpawner.java:62-76, EntityMob.java:92-102)");
	check(ids_used == 4u * (uint32_t)bursts,
		  "d2: every one of the 4 members per burst takes its "
		  "createEntityInWorld id, spawned or refused (Entity.java:54)");

	/* d3 (G6, i05-1005): the crowd count [CFR TileEntityMobSpawner.java:
	 * 66-70] -- the member CLASS (EntityPigZombie extends EntityZombie) whose
	 * box strictly intersects the spawner cell expand(8, 4, 8); >= 6 ENDS the
	 * burst after member 0's createEntityInWorld: exactly one id used. */
	{
		/* R5 population changes the preceding world-stream history. Make
		 * placement impossible so an accepted member cannot itself become
		 * the sixth Zombie and end the class-count probe early. The earlier
		 * dungeon checks retain the real open-room eligibility/own-RNG path. */
		for(int x = sx - 6; x <= sx + 6; x++)
			for(int z = sz - 6; z <= sz + 6; z++)
				for(int y = sy - 1; y <= sy + 3; y++) {
					(void)server_world_set_block_silent(&s.worlds[0], x, y, z,
						(struct block_data) {.type = BLOCK_STONE});
					struct block_data cell;
					assert(server_world_get_block(&s.worlds[0], x, y, z, &cell)
						&& cell.type == BLOCK_STONE);
				}
		const int crowd_case[3][2] = {
			/* {pigmen inside, 1 = a 6th pigman just outside x / 2 = a skeleton} */
			{6, 0}, {5, 1}, {5, 2},
		};
		const uint32_t want_ids[3] = {1u, 4u, 4u};
		for(int c = 0; c < 3; c++) {
			clear_entities();
			for(int k = 0; k < crowd_case[c][0]; k++)
				assert(server_local_spawn_monster_w(&s.worlds[0],
					(vec3) {(float)sx + 0.5F + (float)(k - 2), (float)sy,
							(float)sz + 3.5F}, MONSTER_PIG_ZOMBIE));
			if(crowd_case[c][1] == 1) /* box max x = sx + 9; minX = x - 0.3 */
				assert(server_local_spawn_monster_w(&s.worlds[0],
					(vec3) {(float)sx + 9.3F, (float)sy, (float)sz + 0.5F},
					MONSTER_PIG_ZOMBIE));
			else if(crowd_case[c][1] == 2)
				assert(server_local_spawn_monster_w(&s.worlds[0],
					(vec3) {(float)sx + 3.5F, (float)sy, (float)sz + 0.5F},
					MONSTER_SKELETON));
			const uint32_t before = s.next_entity_id;
			server_local_test_spawner_fire(&s, sx, sy, sz, MONSTER_ZOMBIE);
			const uint32_t used = s.next_entity_id - before;
			printf("  crowd case %d: %u ids used (want %u)\n", c,
				   (unsigned)used, (unsigned)want_ids[c]);
			check(used == want_ids[c],
				  c == 0 ? "d3: 6 pigmen in the Zombie spawner box end the "
						   "burst after member 0's construct"
				  : c == 1 ? "d3: the strict box edge -- a pigman whose minX "
							 "equals maxX is not counted"
						   : "d3: a skeleton is not of the Zombie class");
			check(dict_entity_size(s.entities) == 6,
				"d3: blocked placement preserves the six fixture monsters");
		}
	}
	clear_entities();
}

/* y1 (I04): a natural/sleep candidate is placed with the WORLD stream's
 * yaw nextFloat()*360 right after newInstance (SpawnerAnimals.java:131,
 * :207); its own Random is untouched by it. */
static void placed_yaw_golden(void) {
	struct random_gen world = s.rand_src;
	(void)rand_gen_flt(&world); /* the one world draw expected */
	uint32_t id;
	int mob;
	struct java_random got;
	server_local_test_spawn_candidate_placed(&s, &s.worlds[0],
		MONSTER_ZOMBIE, &id, &mob, &got);
	struct java_random fresh;
	entity_lifetime_rng_seed(&fresh, s.worlds[0].seed, s.worlds[0].dimension,
							 id);
	check(memcmp(&world, &s.rand_src, sizeof world) == 0
			  && fresh.seed == got.seed,
		  "y1: placing a candidate draws exactly one world-stream yaw float "
		  "and nothing from its own Random (SpawnerAnimals.java:131)");
}

static void run_world(enum world_dim dim, int64_t seed, const char* name) {
	char sub[64];
	snprintf(sub, sizeof sub, "/%s", name);
	char path[220];
	snprintf(path, sizeof path, "%s%s", s_dir, sub);
	MKDIR(path);
	char region[240];
	snprintf(region, sizeof region, "%s/region", path);
	MKDIR(region);
	string_t level;
	string_init_set_str(level, path);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, dim, seed, false);
	string_clear(level);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	s.render_demo_mob = -1;
	s.rand_src.seed = 0x51A7E5EDu;
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	for(int cx = -4; cx <= 4; cx++)
		for(int cz = -4; cz <= 4; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 8.5;
	s.player.y = 64.0;
	s.player.z = 8.5;

	int spawned = 0, matched = 0, riders = 0, kinds_seen[64] = {0};
	uint32_t ids_before = s.next_entity_id;
	for(int pass = 0; pass < 1500; pass++) {
		uint32_t before = s.next_entity_id;
		server_local_test_spawn_mobs(&s);
		/* Inspect the mobs this pass created (ids in [before, next)). */
		for(uint32_t id = before; id < s.next_entity_id; id++) {
			struct entity** pp = dict_entity_get(s.entities, id);
			if(!pp || !*pp || (*pp)->type != ENTITY_MONSTER)
				continue;
			struct entity* e = *pp;
			if(e->data.monster.jockey_mount_id != UINT32_MAX) {
				riders++;
				continue; /* a jockey rider: constructed by the jockey roll */
			}
			spawned++;
			struct java_random want;
			if(expected_state(e, &want) && want.seed == e->gameplay_rng.seed)
				matched++;
			int k = e->data.monster.id;
			if(k >= 0 && k < 64)
				kinds_seen[k]++;
		}
		if(pass % 50 == 49)
			clear_entities(); /* keep the caps open */
	}
	uint32_t ids_used = s.next_entity_id - ids_before;
	printf("  %s: %d spawned (zombie %d skeleton %d creeper %d spider %d "
		   "slime %d ghast %d pig-zombie %d), %d match Beta's own-stream "
		   "state, %u ids used\n",
		   name, spawned, kinds_seen[MONSTER_ZOMBIE],
		   kinds_seen[MONSTER_SKELETON], kinds_seen[MONSTER_CREEPER],
		   kinds_seen[MONSTER_SPIDER],
		   kinds_seen[MONSTER_SLIME_SMALL] + kinds_seen[MONSTER_SLIME_MEDIUM]
			   + kinds_seen[MONSTER_SLIME_BIG],
		   kinds_seen[MONSTER_GHAST], kinds_seen[MONSTER_PIG_ZOMBIE], matched,
		   (unsigned)ids_used);
	assert(spawned > 0 && "fixture: the spawn passes produced mobs");
	char what[200];
	snprintf(what, sizeof what,
			 "%s r1: every spawned mob's stream is exactly past Beta's "
			 "constructor + getCanSpawnHere draws (own Random, from its id)",
			 name);
	check(matched == spawned, what);
	snprintf(what, sizeof what,
			 "%s r2: refused candidates consumed entity ids like Beta's "
			 "newInstance (ids used %u > spawned %d + jockey riders %d)",
			 name, (unsigned)ids_used, spawned, riders);
	check((int)ids_used > spawned + riders, what);
	if(dim == WORLD_DIM_OVERWORLD) {
		slime_candidate_golden();
		cart_spill_golden();
		placed_yaw_golden();
		dungeon_spawner_golden();
	}
	clear_entities();
	dict_entity_clear(s.entities);
	server_world_discard(&s.worlds[0]);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	test_fixture_dir("test_spawn_candidate_rng_tmp", s_dir, sizeof s_dir);
	MKDIR(s_dir);
	assert(clin_init());
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	run_world(WORLD_DIM_OVERWORLD, 424242, "overworld");
	run_world(WORLD_DIM_NETHER, 99881, "nether");

	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(s_dir);
	printf("%s test_spawn_candidate_rng (%d failure%s)\n",
		   failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
