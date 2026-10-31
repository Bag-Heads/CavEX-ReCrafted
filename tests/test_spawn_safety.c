/*
	BUG-SPAWN (maintainer 2026-10-14): on seed 12345 the real-Wii perf_stress_5m
	run suffocates the host at (0.5, 84.62, 0.5) every run.

	This drives the PRODUCTION new-world path end to end on PC, no Wii needed:
	  1. the new-world archive exactly as the create-world screen writes it
	     (worldgen_find_initial_spawns -> level.dat with LastPlayed 0, Player
	     .Pos = feet + PLAYER_EYE_HEIGHT);
	  2. server_local_load_world_impl (the first-join push-up runs here);
	  3. production server updates until the load completes (the debug
	     perf_stress load setup, server_local_perf_stress_demo, runs at that
	     moment, as on the console);
	  4. then the 3x3 chunk neighbourhood around the player is populated, the
	     way the streaming loop populates it later on a slow Wii.
	After 2-4 the player must stand clear: no normal cube in the Beta
	isEntityInsideOpaqueBlock probe cells and no full cube in the 0.6 x 1.8
	body box.

	Beta 1.7.3 references (RetroMCP client decompile, mcb173-retromcp):
	  client/Minecraft.java:1270-1296  changeWorld: func_6255_d ("Building
	      terrain", radius 128 around the spawn, every chunk loaded and so
	      populated) runs BEFORE createPlayer + preparePlayerToSpawn.
	  src/Entity.java:96-110           preparePlayerToSpawn push-up.
	  src/Entity.java isEntityInsideOpaqueBlock: 8 probes at
	      posY + getEyeHeight() (0.12F for the player) +-0.05.
	Server (CFR /c/tmp/mcb173-server-decompile/out/net/minecraft/server/
	MinecraftServer.java:181-203): "Preparing start region" loads every chunk
	within 196 blocks of getSpawnPoint() before any player joins.
	So in Beta nothing populated around spawn can land in the player AFTER the
	push-up: population finishes first.

	Seed 42 is the control (no burial before or after).
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
#include "entity/combat.h"
#include "entity/entity.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/level_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "worldgen/worldgen.h"

static struct server_local s_srv;
static int g_failures;

static bool block_at(struct server_world* w, int x, int y, int z,
					 struct block_data* out) {
	return server_world_get_block(w, x, y, z, out);
}

static void dump_column(struct server_world* w, int x, int z, int y0, int y1) {
	printf("    column (%d,%d) y%d..%d:", x, z, y0, y1);
	for(int y = y0; y <= y1; y++) {
		struct block_data b = {0};
		if(block_at(w, x, y, z, &b))
			printf(" %d:%u", y, (unsigned)b.type);
		else
			printf(" %d:?", y);
	}
	uint8_t h = 0;
	if(server_world_get_height(w, x, z, &h))
		printf("  heightmap=%u", (unsigned)h);
	printf("\n");
}

/* Beta Entity.isEntityInsideOpaqueBlock for the player (eye basis posY,
 * getEyeHeight 0.12F, width 0.6F * 0.9F spread, World.isBlockNormalCube). */
static bool inside_opaque(struct server_world* w, double px, double py,
						  double pz) {
	for(int i = 0; i < 8; i++) {
		float dx = ((float)((i >> 0) % 2) - 0.5F) * 0.6F * 0.9F;
		float dy = ((float)((i >> 1) % 2) - 0.5F) * 0.1F;
		float dz = ((float)((i >> 2) % 2) - 0.5F) * 0.6F * 0.9F;
		struct block_data b;
		if(block_at(w, (int)floor(px + dx), (int)floor(py + 0.12F + dy),
					(int)floor(pz + dz), &b)
		   && block_is_normal_cube(b.type))
			return true;
	}
	return false;
}

/* Any normal (full) cube overlapping the 0.6 x 1.8 body box. */
static bool body_in_full_cube(struct server_world* w, double px, double py,
							  double pz) {
	const double feet = py - (double)PLAYER_EYE_HEIGHT;
	const double x1 = px - 0.3, x2 = px + 0.3, z1 = pz - 0.3, z2 = pz + 0.3;
	for(int x = (int)floor(x1); x <= (int)floor(x2); x++)
		for(int z = (int)floor(z1); z <= (int)floor(z2); z++)
			for(int y = (int)floor(feet); (double)y < feet + 1.8; y++) {
				struct block_data b;
				if(block_at(w, x, y, z, &b) && block_is_normal_cube(b.type))
					return true;
			}
	return false;
}

static void populate_neighbourhood(struct server_local* s, int cx0, int cz0) {
	for(int cx = cx0 - 1; cx <= cx0 + 1; cx++)
		for(int cz = cz0 - 1; cz <= cz0 + 1; cz++) {
			struct server_chunk* c = NULL;
			bool ok = false;
			/* Overworld-only spawn fixture: name slot 0 explicitly (the
			 * deprecated `world` alias is the same storage). */
			for(unsigned n = 0; n < 1000 && !ok; n++) {
				server_world_begin_update(&s->worlds[0]);
				ok = server_world_ensure_populated(&s->worlds[0], s, cx, cz, &c)
					&& c;
			}
			assert(ok);
		}
}

static void check_seed(int64_t seed) {
	char dir[128];
	char tag[64];
	snprintf(tag, sizeof(tag), "test_spawn_safety_%lld", (long long)seed);
	test_fixture_dir(tag, dir, sizeof(dir));
	MKDIR(dir);
	MKDIR(test_fixture_join(dir, "/region"));

	/* 1. The create-world screen's archive (screen_select_world.c
	 * sworld_persist_new_archive): SpawnXYZ = world_spawn, Player.Pos = the
	 * generator's feet + PLAYER_EYE_HEIGHT, LastPlayed 0 from the template. */
	vec3 world_spawn, player_spawn;
	worldgen_find_initial_spawns(seed, false, false, world_spawn, player_spawn);
	player_spawn[1] += PLAYER_EYE_HEIGHT;
	string_t lvl;
	string_init_set_str(lvl, dir);
	{
		struct level_archive la;
		assert(level_archive_create_new(&la, lvl, seed, world_spawn));
		assert(level_archive_write_player(&la, player_spawn, (vec2) {0, 0},
										  (vec3) {0, 0, 0},
										  WORLD_DIM_OVERWORLD));
		assert(level_archive_save(&la));
		level_archive_discard(&la);
	}

	/* 2. + 3. The production load and its completion. */
	struct server_local* s = &s_srv;
	memset(s, 0, sizeof(*s));
	server_local_test_create_without_thread(s, false, false, false, false,
											false, false /* superflat */,
											true /* perf_stress */);
	assert(server_local_load_world_impl(s, lvl));
	for(unsigned n = 0; n < 4000 && !s->player.finished_loading; n++) {
		server_local_test_update_once(s);
		clin_drain_discard();
	}
	assert(s->player.finished_loading);
	struct server_world* w = &s->worlds[0];
	const int bx = (int)floor(s->player.x), bz = (int)floor(s->player.z);
	printf("  seed %lld: generator spawn feet (%.1f,%.1f,%.1f); loaded eye "
		   "(%.3f,%.3f,%.3f)\n",
		   (long long)seed, world_spawn[0], world_spawn[1], world_spawn[2],
		   s->player.x, s->player.y, s->player.z);
	dump_column(w, bx, bz, (int)world_spawn[1] - 2, (int)world_spawn[1] + 9);
	if(inside_opaque(w, s->player.x, s->player.y, s->player.z)
	   || body_in_full_cube(w, s->player.x, s->player.y, s->player.z)) {
		g_failures++;
		printf("  FAIL seed %lld: player buried when the load completes "
			   "(eye %.3f)\n", (long long)seed, s->player.y);
	}
	/* The perf_stress load setup stands the FEET on the heightmap cell (the
	 * first cell above the top light-blocking block); both seeds' columns are
	 * clear there, so Beta's push-up leaves it: eye = h + 1.62F. */
	uint8_t h = 0;
	assert(server_world_get_height(w, bx, bz, &h));
	if(s->player.y != (double)(h + PLAYER_EYE_HEIGHT)) {
		g_failures++;
		printf("  FAIL seed %lld: eye %.3f, want feet on the heightmap %u "
			   "(eye %.3f)\n", (long long)seed, s->player.y, (unsigned)h,
			   (double)(h + PLAYER_EYE_HEIGHT));
	}

	/* 4. Population that the streaming loop would finish later. */
	populate_neighbourhood(s, bx >> 4, bz >> 4);
	printf("  after the 3x3 populate:\n");
	dump_column(w, bx, bz, (int)world_spawn[1] - 2, (int)world_spawn[1] + 9);
	if(inside_opaque(w, s->player.x, s->player.y, s->player.z)
	   || body_in_full_cube(w, s->player.x, s->player.y, s->player.z)) {
		g_failures++;
		printf("  FAIL seed %lld: neighbour population buries the player "
			   "(eye %.3f)\n", (long long)seed, s->player.y);
	}

	server_local_unload_world_impl(s, false);
	clin_drain_discard();
	string_clear(lvl);
	test_fixture_dir_cleanup(dir);
}

/* ---- First-join push-up byte-equivalence (perf_stress OFF) ---------------
 * server_local_first_join_push_up's Beta preparePlayerToSpawn loop was moved
 * into a shared helper for the perf_stress fix. A normal first join (a
 * never-saved level.dat, LastPlayed 0) must give the SAME position bits as
 * before. Superflat world (grass at y3, feet cell 4); one column per case,
 * the Player tag eye = 4 + 1.62F exactly as the create-world screen writes
 * it. The expected bit patterns were recorded from HEAD 1706a7f8's
 * server_local.c (tmp/jobs/loop/BUG-SPAWN/firstjoin_head.log). */
/* Recorded from HEAD 1706a7f8 (feet 4 / 6 / 5 / 5, i.e. the Beta push-up
 * results test_spawn_fallback_collision_golden hand-derives for the same
 * blocks): eye = (float)(feet + 1.62F) widened to double. */
#define FJ_BITS_CLEAR 0x40167ae140000000ULL  /* 5.61999989 */
#define FJ_BITS_STONE2 0x401e7ae140000000ULL /* 7.61999989 */
#define FJ_BITS_SLAB 0x401a7ae140000000ULL   /* 6.61999989 */
#define FJ_BITS_SNOW3 0x401a7ae140000000ULL  /* 6.61999989 */

struct first_join_case {
	const char* name;
	int x, z;
	int feet_type, feet_meta; /* block at y4 */
	int head_type;            /* block at y5 */
	uint64_t want_bits;       /* s->player.y after the load (double bits) */
};

static uint64_t double_bits(double d) {
	uint64_t u;
	memcpy(&u, &d, sizeof(u));
	return u;
}

static const struct first_join_case FIRST_JOIN[] = {
	{"clear", 8, 8, BLOCK_AIR, 0, BLOCK_AIR, FJ_BITS_CLEAR},
	{"obstructed (stone y4+y5)", 10, 8, BLOCK_STONE, 0, BLOCK_STONE,
	 FJ_BITS_STONE2},
	{"partial (slab y4)", 12, 8, BLOCK_SLAB, 0, BLOCK_AIR, FJ_BITS_SLAB},
	{"partial (snow layer meta 3 y4)", 14, 8, BLOCK_SNOW, 3, BLOCK_AIR,
	 FJ_BITS_SNOW3},
};

static void first_join_rewrite_level(string_t lvl, const struct first_join_case* c) {
	struct level_archive la;
	assert(level_archive_create(&la, lvl));
	int64_t never = 0;
	assert(level_archive_write(&la, LEVEL_LAST_PLAYED, &never));
	assert(level_archive_write_player(&la,
		(vec3) {(float)c->x + 0.5F, 4 + PLAYER_EYE_HEIGHT, (float)c->z + 0.5F},
		(vec2) {0, 0}, (vec3) {0, 0, 0}, WORLD_DIM_OVERWORLD));
	assert(level_archive_save(&la));
	level_archive_discard(&la);
}

static void check_first_join(void) {
	char dir[128];
	test_fixture_dir("test_spawn_safety_firstjoin", dir, sizeof(dir));
	MKDIR(dir);
	MKDIR(test_fixture_join(dir, "/region"));
	string_t lvl;
	string_init_set_str(lvl, dir);
	{
		struct level_archive la;
		assert(level_archive_create_new(&la, lvl, 4242,
			(vec3) {8.5F, 4.0F, 8.5F}));
		assert(level_archive_save(&la));
		level_archive_discard(&la);
	}
	struct server_local* s = &s_srv;
	memset(s, 0, sizeof(*s));
	server_local_test_create_without_thread(s, false, false, false, false,
											false, true /* superflat */,
											false /* perf_stress OFF */);
	/* Author every case's column once, saved through the normal unload. */
	assert(server_local_load_world_impl(s, lvl));
	struct server_chunk* sc = NULL;
	if(!dict_server_chunks_get(s->worlds[0].chunks, S_CHUNK_ID(0, 0)))
		assert(server_world_load_chunk(&s->worlds[0], 0, 0, &sc) && sc);
	for(size_t i = 0; i < sizeof FIRST_JOIN / sizeof FIRST_JOIN[0]; i++) {
		const struct first_join_case* c = &FIRST_JOIN[i];
		struct block_data b;
		assert(block_at(&s->worlds[0], c->x, 3, c->z, &b) && b.type != BLOCK_AIR);
		(void)server_world_set_block(&s->worlds[0], c->x, 4, c->z,
			(struct block_data) {.type = (uint8_t)c->feet_type,
								 .metadata = (uint8_t)c->feet_meta});
		(void)server_world_set_block(&s->worlds[0], c->x, 5, c->z,
			(struct block_data) {.type = (uint8_t)c->head_type});
		assert(block_at(&s->worlds[0], c->x, 4, c->z, &b)
			   && b.type == c->feet_type && b.metadata == c->feet_meta);
		assert(block_at(&s->worlds[0], c->x, 5, c->z, &b)
			   && b.type == c->head_type);
	}
	server_local_unload_world_impl(s, false);
	clin_drain_discard();

	for(size_t i = 0; i < sizeof FIRST_JOIN / sizeof FIRST_JOIN[0]; i++) {
		const struct first_join_case* c = &FIRST_JOIN[i];
		first_join_rewrite_level(lvl, c);
		assert(server_local_load_world_impl(s, lvl));
		struct block_data feet;
		assert(block_at(&s->worlds[0], c->x, 4, c->z, &feet)
			   && feet.type == c->feet_type);
		const uint64_t got = double_bits(s->player.y);
		printf("  first join %-32s x=%.3f y=%.9g z=%.3f y_bits=0x%016llx\n",
			   c->name, s->player.x, s->player.y, s->player.z,
			   (unsigned long long)got);
		if(got != c->want_bits || s->player.x != c->x + 0.5F
		   || s->player.z != c->z + 0.5F) {
			g_failures++;
			printf("  FAIL first join %s: y_bits 0x%016llx, HEAD 0x%016llx\n",
				   c->name, (unsigned long long)got,
				   (unsigned long long)c->want_bits);
		}
		server_local_unload_world_impl(s, false);
		clin_drain_discard();
	}
	string_clear(lvl);
	test_fixture_dir_cleanup(dir);
}

int main(void) {
	puts("test_spawn_safety: a fresh world never buries the player at spawn "
		 "(BUG-SPAWN)");
	assert(clin_init());
	blocks_init();
	items_init();
	check_first_join();
	check_seed(12345);
	check_seed(42);
	if(g_failures) {
		printf("test_spawn_safety: FAIL (%d)\n", g_failures);
		return 1;
	}
	puts("test_spawn_safety: PASS");
	return 0;
}
