/*
	F2 regression: a blast that fails staging allocation must NOT damage the
	player. Before the fix, the second staging malloc ran AFTER player damage,
	knockback, and possible death-and-respawn, so an OOM abort could hurt or
	kill the player and then silently cancel the explosion.

	Sub-test 2 (next-gaps-0924/G2): the collect loop is Beta 1.7.3
	Explosion.doExplosionA (1352 grid rays, resistance attenuation); a hand
	transcription of it runs over the same seed and world as the production
	blast and the carved sets must be identical. (Before G2 this sub-test
	compared the B7 hash-set collect against the OLD O(N^2)+512-cap random-
	ray collect -- a reference that is no longer the model.)

	Both sub-tests use the same fixture as test_explosion_large_tnt: a solid
	dirt monolith on the superflat debug stage, deterministic world RNG.
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
#include <process.h>
#define MKDIR(p) _mkdir(p)
#define GETPID() _getpid()
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define GETPID() getpid()
#endif

#include <m-lib/m-string.h>

#include "block/blocks.h"
#include "block/blocks_data.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "platform/mem2.h"
#include "platform/thread.h"

static char g_tmp_world[64];
#define TMP_WORLD g_tmp_world

static const char* tmp_path(const char* suffix) {
	static char buf[96];
	int n = snprintf(buf, sizeof(buf), "%s%s", g_tmp_world, suffix);
	assert(n > 0 && (size_t)n < sizeof(buf));
	return buf;
}

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

static void free_entities(struct server_local* s) {
	dict_entity_it_t it;
	dict_entity_it(it, s->entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_reset(s->entities);
}

/* ================================================================ */
/* Sub-test 1: fail-before -- staging OOM must not damage the player. */
/* ================================================================ */

#define ABORT_POWER 4.0f
#define ABORT_CX    40
#define ABORT_CZ    40
#define ABORT_CY    8.5f

static void test_blast_abort_no_player_damage(void) {
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	int n = snprintf(g_tmp_world, sizeof(g_tmp_world),
					 "test_explosion_abort_tmp_%d", (int)GETPID());
	assert(n > 0 && (size_t)n < sizeof(g_tmp_world));
	MKDIR(TMP_WORLD);
	MKDIR(tmp_path("/region"));
	remove(tmp_path("/region/r.0.0.mcr"));
	remove(tmp_path("/level.dat"));
	remove(tmp_path("/level.dat_new"));
	remove(tmp_path("/level.dat_old"));

	string_t level;
	string_init_set_str(level, TMP_WORLD);
	struct server_local s;
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 128;
	s.rand_src.seed = UINT32_C(0xAB04742B);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player,
							&s, &s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = ABORT_CX + 3.5;
	s.player.y = 4.5;
	s.player.z = ABORT_CZ + 0.5;
	s.player.health = 160;
	s.player.vel_x = 0.0f;
	s.player.vel_z = 0.0f;
	s.player.hurt_resist_time = 0;
	s.player.last_damage = 0;

	for(int cx = 1; cx <= 3; cx++)
		for(int cz = 1; cz <= 3; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}

	for(int x = ABORT_CX - 8; x <= ABORT_CX + 8; x++)
		for(int z = ABORT_CZ - 8; z <= ABORT_CZ + 8; z++)
			for(int y = 4; y <= 20; y++)
				assert(server_world_set_block(
					&s.worlds[0], x, y, z,
					(struct block_data) {.type = BLOCK_DIRT,
										 .metadata = 0,
										 .sky_light = 0,
										 .torch_light = 0}));

	short pre_health = s.player.health;
	double pre_x = s.player.x, pre_y = s.player.y, pre_z = s.player.z;
	float pre_vx = s.player.vel_x, pre_vz = s.player.vel_z;
	short pre_hurt = s.player.hurt_resist_time;
	short pre_last_dmg = s.player.last_damage;

	server_world_test_fail_blast_mallocs(5);

	server_world_explode(&s.worlds[0], (vec3) {ABORT_CX + 0.5f, ABORT_CY,
									  ABORT_CZ + 0.5f},
						 ABORT_POWER);

	assert(s.player.health == pre_health);
	assert(s.player.x == pre_x);
	assert(s.player.y == pre_y);
	assert(s.player.z == pre_z);
	assert(s.player.vel_x == pre_vx);
	assert(s.player.vel_z == pre_vz);
	assert(s.player.hurt_resist_time == pre_hurt);
	assert(s.player.last_damage == pre_last_dmg);

	printf("  [abort] health=%d pos=(%.1f,%.1f,%.1f) vel=(%.2f,%.2f) "
		   "-- unchanged after OOM abort\n",
		   s.player.health, s.player.x, s.player.y, s.player.z,
		   s.player.vel_x, s.player.vel_z);

	struct block_data blk;
	assert(server_world_get_block(&s.world, ABORT_CX, 8, ABORT_CZ, &blk));
	assert(blk.type == BLOCK_DIRT);

	free_entities(&s);
	inventory_destroy(&s.player.inventory, &s.player);
	server_world_destroy(&s.world);
	mem2_free(s.entity_tick_keys);
	s.entity_tick_keys = NULL;
	dict_entity_clear(s.entities);
	string_clear(level);
	g_drain_run = 0;
	thread_join(&drain);
	g_drain_run = 1;
}

/* ================================================================ */
/* Sub-test 2: identity -- a hand transcription of Beta 1.7.3         */
/* Explosion.doExplosionA's block collection (next-gaps-0924/G2:      */
/* 1352 grid rays, intensity size*(0.7F+0.6F*nextFloat), step 0.3,   */
/* (getExplosionResistance+0.3F)*0.3F per solid cell) run over the    */
/* same seed/world as the production blast; the carved sets must be   */
/* identical. The pre-G2 reference here was the OLD 300-random-ray    */
/* collect (docs/deviations.md #4), which is no longer the model.     */
/* [740c583 Explosion.doExplosionA:29-75]                              */
/* ================================================================ */

struct ref_coord { int x, y, z; };

/* Block.getExplosionResistance for the ids the dirt fixture can meet:
 * dirt setHardness(0.5F) -> 2.5F/5; the superflat ground under it
 * (grass 0.6F, dirt, bedrock unbreakable+setResistance(6000000.0F)).
 * [740c583 Block.java:25-30, setHardness:199, setResistance:186] */
static float ref_resistance(uint8_t id) {
	float blockResistance = 0.0F;
	float hardness;
	switch(id) {
		case BLOCK_DIRT: hardness = 0.5F; break;
		case BLOCK_GRASS: hardness = 0.6F; break;
		case BLOCK_BEDROCK:
			return (6000000.0F * 3.0F) / 5.0F;
		default:
			assert(id == BLOCK_AIR);
			return 0.0F;
	}
	if(blockResistance < hardness * 5.0F)
		blockResistance = hardness * 5.0F;
	return blockResistance / 5.0F;
}

/* Beta doExplosionA over the LIVE pre-blast world (read-only). Returns the
 * non-air cells inside the ID window, first-seen order; caller frees. */
static struct ref_coord* ref_beta_collect(struct server_local* s,
										  vec3 center, float power,
										  size_t* out_count);

#define ID_POWER 4.0f
#define ID_CX    60
#define ID_CZ    60
#define ID_CY    8.5f
#define ID_MIN_X (ID_CX - 5)
#define ID_MAX_X (ID_CX + 5)
#define ID_MIN_Z (ID_CZ - 5)
#define ID_MAX_Z (ID_CZ + 5)
#define ID_MIN_Y 4
#define ID_MAX_Y 14
#define ID_NX (ID_MAX_X - ID_MIN_X + 1)
#define ID_NY (ID_MAX_Y - ID_MIN_Y + 1)
#define ID_NZ (ID_MAX_Z - ID_MIN_Z + 1)

static struct ref_coord* ref_beta_collect(struct server_local* s,
										  vec3 center, float power,
										  size_t* out_count) {
	static uint8_t marked[ID_NX * ID_NY * ID_NZ];
	memset(marked, 0, sizeof(marked));
	const double explosionX = (double)center[0];
	const double explosionY = (double)center[1];
	const double explosionZ = (double)center[2];
	const int var2 = 16;
	for(int var3 = 0; var3 < var2; ++var3)
		for(int var4 = 0; var4 < var2; ++var4)
			for(int var5 = 0; var5 < var2; ++var5) {
				if(!(var3 == 0 || var3 == var2 - 1 || var4 == 0
					 || var4 == var2 - 1 || var5 == 0 || var5 == var2 - 1))
					continue;
				double var6 = (double)((float)var3 / ((float)var2 - 1.0F) * 2.0F - 1.0F);
				double var8 = (double)((float)var4 / ((float)var2 - 1.0F) * 2.0F - 1.0F);
				double var10 = (double)((float)var5 / ((float)var2 - 1.0F) * 2.0F - 1.0F);
				double var12 = sqrt(var6 * var6 + var8 * var8 + var10 * var10);
				var6 /= var12;
				var8 /= var12;
				var10 /= var12;
				float var14 = power * (0.7F + jrand_next_float(&s->worlds[0].rand) * 0.6F); /* F4-RNG: World.rand */
				double var15 = explosionX, var17 = explosionY, var19 = explosionZ;
				for(float var21 = 0.3F; var14 > 0.0F; var14 -= var21 * (12.0F / 16.0F)) {
					int var22 = (int)floor(var15);
					int var23 = (int)floor(var17);
					int var24 = (int)floor(var19);
					struct block_data blk;
					uint8_t var25 = 0;
					if(server_world_get_block(&s->world, var22, var23, var24, &blk))
						var25 = blk.type;
					if(var25 > 0)
						var14 -= (ref_resistance(var25) + 0.3F) * var21;
					if(var14 > 0.0F && var25 > 0 && var22 >= ID_MIN_X
					   && var22 <= ID_MAX_X && var23 >= ID_MIN_Y && var23 <= ID_MAX_Y
					   && var24 >= ID_MIN_Z && var24 <= ID_MAX_Z)
						marked[((size_t)(var22 - ID_MIN_X) * ID_NY + (var23 - ID_MIN_Y))
							   * ID_NZ + (var24 - ID_MIN_Z)] = 1;
					var15 += var6 * (double)var21;
					var17 += var8 * (double)var21;
					var19 += var10 * (double)var21;
				}
			}
	struct ref_coord* broken = malloc(ID_NX * ID_NY * ID_NZ * sizeof(*broken));
	assert(broken);
	size_t bc = 0;
	for(int x = ID_MIN_X; x <= ID_MAX_X; x++)
		for(int y = ID_MIN_Y; y <= ID_MAX_Y; y++)
			for(int z = ID_MIN_Z; z <= ID_MAX_Z; z++)
				if(marked[((size_t)(x - ID_MIN_X) * ID_NY + (y - ID_MIN_Y)) * ID_NZ
						  + (z - ID_MIN_Z)]) {
					broken[bc].x = x;
					broken[bc].y = y;
					broken[bc].z = z;
					bc++;
				}
	*out_count = bc;
	return broken;
}


static void test_blast_byte_identity(void) {
	struct thread drain;
	assert(thread_create(&drain, drain_entry, NULL, 64));
	blocks_init();
	items_init();

	int n = snprintf(g_tmp_world, sizeof(g_tmp_world),
					 "test_explosion_identity_tmp_%d", (int)GETPID());
	assert(n > 0 && (size_t)n < sizeof(g_tmp_world));
	MKDIR(TMP_WORLD);
	MKDIR(tmp_path("/region"));
	remove(tmp_path("/region/r.0.0.mcr"));
	remove(tmp_path("/level.dat"));
	remove(tmp_path("/level.dat_new"));
	remove(tmp_path("/level.dat_old"));

	string_t level;
	string_init_set_str(level, TMP_WORLD);
	struct server_local s;
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 128;
	s.rand_src.seed = UINT32_C(0x1D37117E);
	dict_entity_init(s.entities);
	assert(inventory_create(&s.player.inventory, &inventory_logic_player,
							&s, &s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.x = 200.5;
	s.player.y = 4.5;
	s.player.z = 200.5;

	for(int cx = 1; cx <= 4; cx++)
		for(int cz = 1; cz <= 4; cz++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);
		}

	for(int x = ID_MIN_X - 1; x <= ID_MAX_X + 1; x++)
		for(int z = ID_MIN_Z - 1; z <= ID_MAX_Z + 1; z++)
			for(int y = 4; y <= ID_MAX_Y + 1; y++)
				assert(server_world_set_block(
					&s.worlds[0], x, y, z,
					(struct block_data) {.type = BLOCK_DIRT,
										 .metadata = 0,
										 .sky_light = 0,
										 .torch_light = 0}));

	/* Snapshot pre-blast state. */
	static uint8_t pre[ID_NX * ID_NY * ID_NZ];
	for(int x = ID_MIN_X; x <= ID_MAX_X; x++)
		for(int y = ID_MIN_Y; y <= ID_MAX_Y; y++)
			for(int z = ID_MIN_Z; z <= ID_MAX_Z; z++) {
				struct block_data bd;
				assert(server_world_get_block(&s.world, x, y, z, &bd));
				size_t idx = ((size_t)(x - ID_MIN_X) * ID_NY
							  + (y - ID_MIN_Y)) * ID_NZ + (z - ID_MIN_Z);
				pre[idx] = bd.type;
			}

	/* 1. Save RNG state, run the Beta transcription as reference. */
	/* F4-RNG: the rays draw the exploding world's own World.rand. */
	struct java_random saved_rng = s.worlds[0].rand;
	size_t ref_count = 0;
	struct ref_coord* ref_broken = ref_beta_collect(
		&s, (vec3) {ID_CX + 0.5f, ID_CY, ID_CZ + 0.5f}, ID_POWER,
		&ref_count);

	/* 1b. Run the reference a second time with the same RNG to verify
	 *     the broken[] sequence is element-for-element deterministic. */
	s.worlds[0].rand = saved_rng;
	size_t ref2_count = 0;
	struct ref_coord* ref2_broken = ref_beta_collect(
		&s, (vec3) {ID_CX + 0.5f, ID_CY, ID_CZ + 0.5f}, ID_POWER,
		&ref2_count);
	assert(ref2_count == ref_count);
	for(size_t i = 0; i < ref_count; i++) {
		assert(ref_broken[i].x == ref2_broken[i].x);
		assert(ref_broken[i].y == ref2_broken[i].y);
		assert(ref_broken[i].z == ref2_broken[i].z);
	}
	free(ref2_broken);

	/* 2. Restore RNG so server_world_explode runs the NEW collect with
	 *    the exact same RNG stream. */
	s.worlds[0].rand = saved_rng;

	printf("  [identity] Beta transcription: %zu blocks (deterministic)\n", ref_count);
	assert(ref_count > 0);

	/* 3. Run the actual blast (new collect + mutate). */
	server_world_explode(&s.worlds[0], (vec3) {ID_CX + 0.5f, ID_CY,
									  ID_CZ + 0.5f},
						 ID_POWER);

	/* 4. Build the actual broken set from the world (DIRT -> AIR). */
	size_t actual_count = 0;
	struct ref_coord actual_broken[ID_NX * ID_NY * ID_NZ];
	for(int x = ID_MIN_X; x <= ID_MAX_X; x++)
		for(int y = ID_MIN_Y; y <= ID_MAX_Y; y++)
			for(int z = ID_MIN_Z; z <= ID_MAX_Z; z++) {
				size_t idx = ((size_t)(x - ID_MIN_X) * ID_NY
							  + (y - ID_MIN_Y)) * ID_NZ + (z - ID_MIN_Z);
				struct block_data bd;
				assert(server_world_get_block(&s.world, x, y, z, &bd));
				if(pre[idx] != BLOCK_AIR && bd.type == BLOCK_AIR) {
					actual_broken[actual_count].x = x;
					actual_broken[actual_count].y = y;
					actual_broken[actual_count].z = z;
					actual_count++;
				}
			}

	printf("  [identity] new collect: %zu blocks (actual broken)\n",
		   actual_count);

	/* 5. Assert counts match. */
	assert(actual_count == ref_count);

	/* 6. Assert the SETS match element-for-element: every reference
	 *    block is in the actual broken set and vice versa.
	 *
	 *    The test proves SET equality by reconstruction from the world
	 *    (step 4), which has no ordering. Sequence order is not directly
	 *    compared here. However, order equality follows from set equality
	 *    plus identical traversal: both the old and new collect append
	 *    in ray/step order — the new hash set only answers "inserted?",
	 *    the append is still in traversal order
	 *    (server_world.c:3940-3956 at 43bbb01), exactly as the old O(N^2)
	 *    dedup did (server_world.c:3816-3820 at 43bbb01^ = 58702fc). This
	 *    matters because pass 3 draws the drop RNG in broken[] order, so
	 *    the same set in a different order would yield different drops. */
	for(size_t i = 0; i < ref_count; i++) {
		bool found = false;
		for(size_t j = 0; j < actual_count; j++) {
			if(actual_broken[j].x == ref_broken[i].x
			   && actual_broken[j].y == ref_broken[i].y
			   && actual_broken[j].z == ref_broken[i].z) {
				found = true;
				break;
			}
		}
		assert(found);
	}
	for(size_t i = 0; i < actual_count; i++) {
		bool found = false;
		for(size_t j = 0; j < ref_count; j++) {
			if(ref_broken[j].x == actual_broken[i].x
			   && ref_broken[j].y == actual_broken[i].y
			   && ref_broken[j].z == actual_broken[i].z) {
				found = true;
				break;
			}
		}
		assert(found);
	}

	free(ref_broken);

	printf("  [identity] sets match: %zu blocks\n", ref_count);
	printf("  [identity] pass 3 drops: %zu item entities spawned\n",
		   dict_entity_size(s.entities));
	assert(dict_entity_size(s.entities) > 0);

	free_entities(&s);
	inventory_destroy(&s.player.inventory, &s.player);
	server_world_destroy(&s.world);
	mem2_free(s.entity_tick_keys);
	s.entity_tick_keys = NULL;
	dict_entity_clear(s.entities);
	string_clear(level);
	g_drain_run = 0;
	thread_join(&drain);
}

int main(void) {
	assert(clin_init());
	assert(svin_init());

	printf("=== test_explosion_blast_abort: sub-test 1 (OOM abort) ===\n");
	test_blast_abort_no_player_damage();
	printf("  sub-test 1: PASS\n");

	printf("=== test_explosion_blast_abort: sub-test 2 (byte-identity) ===\n");
	test_blast_byte_identity();
	printf("  sub-test 2: PASS\n");

	printf("test_explosion_blast_abort: OK (OOM abort leaves player untouched, "
		   "collect output element-for-element identical to old path)\n");
	return 0;
}
