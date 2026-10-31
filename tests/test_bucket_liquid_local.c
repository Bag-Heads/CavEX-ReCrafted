/*
	BUCKET-WATER-OFFLINE (card P2): in local/split play a bucket aimed at
	water/lava did nothing -- the crosshair ray passes through liquids, so the
	click is a NO-TARGET use (SRPC_USE_ITEM), and server_local had no bucket
	branch for it. Beta 1.7.3 ItemBucket.onItemRightClick does its OWN ray
	[mcb173-retromcp ItemBucket.java:12-99]:
	  :13-27  eye origin, MathHelper look vector, 5.0D reach;
	  :28     rayTraceBlocks_do(start, end, isFull == 0) -- liquids hittable
	          only for the EMPTY bucket, and BlockFluid.canCollideCheck
	          (`var2 && var1 == 0`, BlockFluid.java:56-58) makes only SOURCES
	          hittable;
	  :41-49  empty: water/lava material at metadata 0 -> air, filled bucket;
	  :51-53  milk (isFull -1): any tile hit -> empty bucket;
	  :55-90  full: step to sideHit's face cell; air or non-solid -> place
	          isFull (water/lava MOVING, meta 0), or in the nether a water
	          bucket fizzes and places nothing; both return the empty bucket.
	PlayerControllerSP.sendUseItem writes the returned stack over the slot.
	Creative (net-new, docs/creative_mode.md) keeps the held stack, exactly
	like the SRPC_BLOCK_PLACE path.

	Drives the REAL dispatch (server_local_test_process_rpc, SRPC_USE_ITEM):
	  B1 empty bucket picks up a water source; B2 a lava source;
	  B3 no pickup from flowing water (ray passes through to the stone);
	  B4 water / lava bucket places at the hit face (down, and a -Z wall face);
	     a full bucket's ray passes through flowing water and replaces it;
	  B5 nether: the water bucket evaporates (no block, bucket emptied), lava
	     still places;
	  B6 creative keeps the held bucket (world still changes); survival swaps
	     the whole slot (count 1);
	  B7 milk on a tile -> empty bucket; any bucket aimed at the sky is a no-op.
	  B8 AIMED at a block (SRPC_BLOCK_PLACE): ItemBucket has no onItemUse, so
	     Beta still runs onItemRightClick's own ray -- a block behind still
	     water picks up the water; the placement face comes from the bucket
	     ray, not the crosshair block/face; creative still keeps the bucket.
*/
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#include "entity/entity.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/server_world.h"
#include "platform/thread.h"

static char s_world[128];
/* Overworld slot only; every access names &s.worlds[0] explicitly. */
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

#define Y0 100 /* floor height: open air well above the 4242 terrain */

static void hold(uint16_t id) {
	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR,
					   (struct item_data) {.id = id, .count = 1});
}
static struct item_data held(void) {
	struct item_data it = {0};
	inventory_get_hotbar_item(&s.player.inventory, &it);
	return it;
}
static void set(int x, int y, int z, uint8_t type, uint8_t meta) {
	/* server_world_set_block refuses an unchanged id+meta write. */
	struct block_data old;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &old));
	if(old.type == type && old.metadata == meta)
		return;
	assert(server_world_set_block(&s.worlds[0], x, y, z,
		(struct block_data) {.type = type, .metadata = meta,
							 .sky_light = 15}));
}
static struct block_data get(int x, int y, int z) {
	struct block_data b;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &b));
	return b;
}
/* Empty 5x6x7 box around the column, then a stone floor at (0, Y0, 0). */
static void reset_scene(void) {
	for(int x = -2; x <= 2; x++)
		for(int y = Y0 - 1; y <= Y0 + 6; y++)
			for(int z = -2; z <= 4; z++)
				set(x, y, z, BLOCK_AIR, 0);
	set(0, Y0, 0, BLOCK_STONE, 0);
}
/* Eye 3 blocks above the floor top, looking straight down (Beta pitch 90). */
static void look_down(void) {
	s.player.x = 0.5;
	s.player.y = Y0 + 1 + 3.0;
	s.player.z = 0.5;
	s.player.rx = 0.0F;
	s.player.ry = 90.0F;
}
static void use_no_target(void) {
	struct item_data h = held();
	struct server_rpc use = {.type = SRPC_USE_ITEM,
		.payload.use_item = {.charge = 1.0F, .held_item = h,
							 .has_held_item = h.count > 0}};
	server_local_test_process_rpc(&s, &use);
}

static void test_pickup_sources(void) {
	reset_scene();
	look_down();
	set(0, Y0 + 1, 0, BLOCK_WATER_STILL, 0);
	hold(ITEM_BUCKET);
	use_no_target();
	assert(held().id == ITEM_BUCKET_WATER && held().count == 1);
	assert(get(0, Y0 + 1, 0).type == BLOCK_AIR && "water source removed");

	reset_scene();
	set(0, Y0 + 1, 0, BLOCK_WATER_FLOW, 0); /* a just-poured source (FLOW, meta 0) */
	hold(ITEM_BUCKET);
	use_no_target();
	assert(held().id == ITEM_BUCKET_WATER && get(0, Y0 + 1, 0).type == BLOCK_AIR);
	puts("PASS B1 empty bucket + water source (still / moving meta 0) -> water bucket, source gone");

	reset_scene();
	set(0, Y0 + 1, 0, BLOCK_LAVA_STILL, 0);
	hold(ITEM_BUCKET);
	use_no_target();
	assert(held().id == ITEM_BUCKET_LAVA && held().count == 1);
	assert(get(0, Y0 + 1, 0).type == BLOCK_AIR && "lava source removed");
	puts("PASS B2 empty bucket + lava source -> lava bucket, source gone");
}

static void test_no_pickup_flowing(void) {
	reset_scene();
	look_down();
	set(0, Y0 + 1, 0, BLOCK_WATER_FLOW, 3);
	hold(ITEM_BUCKET);
	use_no_target();
	assert(held().id == ITEM_BUCKET && "flowing water is not scooped");
	assert(get(0, Y0 + 1, 0).type == BLOCK_WATER_FLOW
		   && get(0, Y0 + 1, 0).metadata == 3);

	set(0, Y0 + 1, 0, BLOCK_WATER_STILL, 2); /* settled partial, not a source */
	use_no_target();
	assert(held().id == ITEM_BUCKET && get(0, Y0 + 1, 0).type == BLOCK_WATER_STILL);

	set(0, Y0 + 1, 0, BLOCK_LAVA_FLOW, 2);
	use_no_target();
	assert(held().id == ITEM_BUCKET && get(0, Y0 + 1, 0).type == BLOCK_LAVA_FLOW);
	puts("PASS B3 no pickup from flowing / partial liquid (ray passes to the stone)");
}

static void test_place_at_face(void) {
	reset_scene();
	look_down();
	hold(ITEM_BUCKET_WATER);
	use_no_target();
	struct block_data b = get(0, Y0 + 1, 0);
	assert(b.type == BLOCK_WATER_FLOW && b.metadata == 0
		   && "waterMoving meta 0 on the top face");
	assert(held().id == ITEM_BUCKET && held().count == 1);

	reset_scene();
	hold(ITEM_BUCKET_LAVA);
	use_no_target();
	b = get(0, Y0 + 1, 0);
	assert(b.type == BLOCK_LAVA_FLOW && b.metadata == 0);
	assert(held().id == ITEM_BUCKET);

	/* The full bucket ray ignores liquids: it passes through flowing water
	 * to the stone and replaces the (non-solid) water cell with a source. */
	reset_scene();
	set(0, Y0 + 1, 0, BLOCK_WATER_FLOW, 5);
	hold(ITEM_BUCKET_WATER);
	use_no_target();
	b = get(0, Y0 + 1, 0);
	assert(b.type == BLOCK_WATER_FLOW && b.metadata == 0);
	assert(held().id == ITEM_BUCKET);

	/* Horizontal: yaw 0 looks +Z [ItemBucket.java:20-25]; a wall at z=3
	 * is hit on sideHit 2 (-Z), so the water lands at z=2. */
	reset_scene();
	set(0, Y0 + 1, 3, BLOCK_STONE, 0);
	s.player.x = 0.5;
	s.player.y = Y0 + 1.5;
	s.player.z = 0.5;
	s.player.rx = 0.0F;
	s.player.ry = 0.0F;
	hold(ITEM_BUCKET_WATER);
	use_no_target();
	assert(get(0, Y0 + 1, 2).type == BLOCK_WATER_FLOW && "placed on the -Z face");
	assert(get(0, Y0 + 1, 3).type == BLOCK_STONE);
	assert(held().id == ITEM_BUCKET);
	puts("PASS B4 water / lava bucket places moving source at the hit face "
		 "(top, -Z wall, through flowing water)");
}

static void test_nether(void) {
	s.worlds[0].dimension = WORLD_DIM_NETHER;
	reset_scene();
	look_down();
	hold(ITEM_BUCKET_WATER);
	use_no_target();
	assert(get(0, Y0 + 1, 0).type == BLOCK_AIR && "nether water evaporates");
	assert(held().id == ITEM_BUCKET && "bucket still emptied");

	hold(ITEM_BUCKET_LAVA);
	use_no_target();
	assert(get(0, Y0 + 1, 0).type == BLOCK_LAVA_FLOW && "lava places in the nether");
	assert(held().id == ITEM_BUCKET);
	s.worlds[0].dimension = WORLD_DIM_OVERWORLD;
	puts("PASS B5 nether: water bucket fizzes (nothing placed, bucket emptied); lava places");
}

static void test_creative_vs_survival(void) {
	s.creative_mode = true;
	reset_scene();
	look_down();
	set(0, Y0 + 1, 0, BLOCK_WATER_STILL, 0);
	hold(ITEM_BUCKET);
	use_no_target();
	assert(get(0, Y0 + 1, 0).type == BLOCK_AIR && "creative still scoops");
	assert(held().id == ITEM_BUCKET && held().count == 1
		   && "creative keeps the empty bucket");

	hold(ITEM_BUCKET_LAVA);
	use_no_target();
	assert(get(0, Y0 + 1, 0).type == BLOCK_LAVA_FLOW && "creative still pours");
	assert(held().id == ITEM_BUCKET_LAVA && held().count == 1
		   && "creative keeps the full bucket");
	s.creative_mode = false;

	/* Survival: the whole slot is swapped, one bucket in, one bucket out. */
	reset_scene();
	set(0, Y0 + 1, 0, BLOCK_LAVA_STILL, 0);
	hold(ITEM_BUCKET);
	use_no_target();
	assert(held().id == ITEM_BUCKET_LAVA && held().count == 1);
	use_no_target(); /* pour it back: the stone top face is free again */
	assert(held().id == ITEM_BUCKET && held().count == 1);
	assert(get(0, Y0 + 1, 0).type == BLOCK_LAVA_FLOW);
	puts("PASS B6 creative keeps the held bucket (world changes); survival swaps the slot 1:1");
}

static void test_milk_and_miss(void) {
	reset_scene();
	look_down();
	hold(ITEM_MILK_BUCKET);
	use_no_target();
	assert(held().id == ITEM_BUCKET && held().count == 1 && "milk poured on a tile");
	assert(get(0, Y0 + 1, 0).type == BLOCK_AIR && "milk places nothing");

	/* Aimed at the sky: var24 == null -> return var1 for every bucket. */
	s.player.ry = -90.0F;
	const uint16_t ids[] = {ITEM_BUCKET, ITEM_BUCKET_WATER, ITEM_BUCKET_LAVA,
		ITEM_MILK_BUCKET};
	for(size_t i = 0; i < sizeof ids / sizeof ids[0]; i++) {
		hold(ids[i]);
		use_no_target();
		assert(held().id == ids[i] && held().count == 1);
	}
	/* Out of reach: the water top is 5.5 blocks below the eye (reach 5.0D). */
	reset_scene();
	look_down();
	s.player.y = Y0 + 2 + 5.5;
	set(0, Y0 + 1, 0, BLOCK_WATER_STILL, 0);
	hold(ITEM_BUCKET);
	use_no_target();
	assert(held().id == ITEM_BUCKET && get(0, Y0 + 1, 0).type == BLOCK_WATER_STILL
		   && "5.5 blocks: out of reach");
	puts("PASS B7 milk on a tile -> empty bucket; sky / out-of-reach use is a no-op");
}

static void click_block(int x, int y, int z, enum side side) {
	struct item_data h = held();
	struct server_rpc place = {.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = side,
								.held_item = h, .has_held_item = true}};
	server_local_test_process_rpc(&s, &place);
}

static void test_aimed_at_block(void) {
	/* Crosshair on the stone floor UNDER a still water source (the ordinary
	 * pick passes through liquids): the bucket ray stops on the source. */
	reset_scene();
	look_down();
	set(0, Y0 + 1, 0, BLOCK_WATER_STILL, 0);
	hold(ITEM_BUCKET);
	click_block(0, Y0, 0, SIDE_TOP);
	assert(held().id == ITEM_BUCKET_WATER && held().count == 1);
	assert(get(0, Y0 + 1, 0).type == BLOCK_AIR && get(0, Y0, 0).type == BLOCK_STONE
		   && "aimed at the block behind: the water is scooped");

	/* Face from the bucket ray: crosshair claims the stone's +Z face, the ray
	 * (straight down) hits its TOP -> water above, nothing at +Z. */
	reset_scene();
	hold(ITEM_BUCKET_WATER);
	click_block(0, Y0, 0, SIDE_BACK);
	assert(get(0, Y0 + 1, 0).type == BLOCK_WATER_FLOW && "placed on the ray's face");
	assert(get(0, Y0, 1).type == BLOCK_AIR && "not on the crosshair's face");
	assert(held().id == ITEM_BUCKET);

	/* Crosshair on a block the ray does not reach (out of 5.0 reach): Beta's
	 * ray misses, so nothing happens even though a block was clicked. */
	reset_scene();
	s.player.y = Y0 + 1 + 5.5;
	hold(ITEM_BUCKET_LAVA);
	click_block(0, Y0, 0, SIDE_TOP);
	assert(held().id == ITEM_BUCKET_LAVA && get(0, Y0 + 1, 0).type == BLOCK_AIR);

	/* Creative, aimed: the world changes, the held bucket does not. */
	s.creative_mode = true;
	reset_scene();
	look_down();
	set(0, Y0 + 1, 0, BLOCK_LAVA_STILL, 0);
	hold(ITEM_BUCKET);
	click_block(0, Y0, 0, SIDE_TOP);
	assert(get(0, Y0 + 1, 0).type == BLOCK_AIR && held().id == ITEM_BUCKET);
	s.creative_mode = false;
	puts("PASS B8 aimed at a block: bucket ray scoops the water in front, places on "
		 "the ray face (not the crosshair face), misses out of reach; creative keeps");
}

int main(void) {
	test_fixture_dir("test_bucket_liquid_local", s_world, sizeof s_world);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	MKDIR(s_world);
	MKDIR(test_fixture_join(s_world, "/region"));
	string_t level;
	string_init_set_str(level, s_world);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true);
	s.worlds[0].session = &s;
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	for(int cz = -1; cz <= 1; cz++)
		for(int cx = -1; cx <= 1; cx++) {
			struct server_chunk* sc = NULL;
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
		}
	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.active_inventory = &s.player.inventory;
	s.player.health = MAX_PLAYER_HEALTH;
	inventory_set_hotbar(&s.player.inventory, 0);

	test_pickup_sources();
	test_no_pickup_flowing();
	test_place_at_face();
	test_nether();
	test_creative_vs_survival();
	test_milk_and_miss();
	test_aimed_at_block();

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	test_fixture_dir_cleanup(s_world);
	puts("test_bucket_liquid_local: ALL OK");
	return 0;
}
