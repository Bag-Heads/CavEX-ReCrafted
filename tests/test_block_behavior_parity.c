/*
	Faithfulness goldens for the Beta 1.7.3 block-behavior gaps fixed from the 2026-06-19 audit, over a
	real superflat server_world + the block registry:
	  - REED water-adjacency (reed_can_block_stay): dirt/grass needs adjacent water; reed-on-reed is fine;
	    non-soil never holds (Beta BlockReed.canPlaceBlockAt/canBlockStay).
	  - CAKE eating (block_cake.onRightClick): 6 bites advance metadata 0..5 then remove the cake, heal
	    24/bite (Beta heal(3) at CaveWii's 8x scale), no-op at full health (Beta `health < 20`).
	  - FARMLAND hydration (block_farmland.onRandomTick): water within the 9x9x2 box -> moisture 7;
	    otherwise dries out and, at 0 with no crop above, reverts to dirt (Beta BlockFarmland.updateTick).
  - FARMLAND trample (Beta BlockFarmland.onEntityWalking + Entity.moveEntity footstep): the
    server_player_walk_step accumulator fires one footstep per distanceWalked/nextStepDistance
    crossing on solid ground; a footstep on farmland reverts it to dirt 1-in-4, popping any crop
    above (BlockCrops canBlockStay support clause -> block_crops.onNeighbourBlockChange).
  - CROPS drop distribution (Beta BlockCrops.dropBlockAsItemWithChance/idDropped): 3 independent
    seed rolls, each dropping a seed iff rand.nextInt(15) <= metadata (0..3 seeds scaling with
    growth, at ANY stage), PLUS exactly 1 wheat iff metadata == 7. Distribution-faithful (rand_gen
    is not byte-exact to Beta's nextInt).
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
#include "daytime.h"
#include "block/blocks_data.h"
#include "entity/combat.h"
#include "entity/entity.h" // entity_soulsand_slowdown golden
#include "game/game_state.h"
#include "game/gui/screen.h"
#include "item/items.h"	   // food onItemPlace heal values + maxstack
#include "network/inventory_logic.h" // inventory_logic_player (bucket golden)
#include "network/client_interface.h"
#include "network/server_interface.h" // MAX_PLAYER_HEALTH
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "lighting.h"
#include "platform/input.h"
#include "platform/thread.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_block_behavior_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define Y 70

static struct server_local s;
static volatile int g_drain_run = 1;
static int g_notify_order[6][3];
static size_t g_notify_order_count;
static int g_lifecycle_stage;
static uint8_t g_lifecycle_old_type;
static uint8_t g_lifecycle_old_meta;
static uint8_t g_lifecycle_new_type;
static uint8_t g_lifecycle_visible_meta;
static uint8_t g_lifecycle_added_meta;
static bool g_component_watch;
static int g_component_x, g_component_y, g_component_z;
static int g_component_notifies;
static uint8_t g_component_origin;

struct sleep_rpc_capture {
	struct client_rpc calls[4];
	size_t count;
};

static void capture_sleep_rpcs(const struct client_rpc* call, void* user) {
	struct sleep_rpc_capture* capture = user;
	assert(capture->count < sizeof(capture->calls) / sizeof(*capture->calls));
	capture->calls[capture->count++] = *call;
}

static int gid(int x, int y, int z);
static int gmeta(int x, int y, int z);
static void* drain_entry(void* arg) {
	(void)arg;
	while(g_drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}
static void set(int x, int y, int z, int type, int meta) {
	bool changed = server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type,
						 .metadata = (uint8_t)(meta & 0x0F)});
	if(!changed) {
		/* Fixture setup is idempotent. Production correctly returns false for an
		 * identical ID+metadata write; reject only an unresident/mismatched cell. */
		struct block_data live;
		assert(server_world_get_block(&s.worlds[0], x, y, z, &live));
		assert(live.type == (uint8_t)type);
		assert(live.metadata == (uint8_t)(meta & 0x0F));
	}
}

static void record_notify_order(struct server_local* server,
								struct block_info* info) {
	(void)server;
	assert(g_notify_order_count < 6);
	g_notify_order[g_notify_order_count][0] = info->x;
	g_notify_order[g_notify_order_count][1] = info->y;
	g_notify_order[g_notify_order_count][2] = info->z;
	g_notify_order_count++;
}

static void record_block_removal(struct server_local* server,
								 struct block_info* info) {
	assert(g_lifecycle_stage == 0);
	assert(info->block->type == g_lifecycle_old_type);
	assert(info->block->metadata == g_lifecycle_old_meta);
	struct block_data live;
	assert(server_world_get_block(
		&server->world, info->x, info->y, info->z, &live));
	/* Chunk.setBlockIDWithMetadata writes the replacement ID first, while the
	 * old metadata nibble remains visible throughout onBlockRemoval. */
	assert(live.type == g_lifecycle_new_type);
	assert(live.metadata == g_lifecycle_visible_meta);
	g_lifecycle_stage = 1;
}

static void record_block_added(struct server_local* server,
							   struct block_info* info) {
	assert(g_lifecycle_stage == 1);
	struct block_data live;
	assert(server_world_get_block(
		&server->world, info->x, info->y, info->z, &live));
	assert(live.type == g_lifecycle_new_type);
	assert(live.metadata == g_lifecycle_added_meta);
	g_lifecycle_stage = 2;
}

static void record_component_notify(
	struct server_local* server, struct block_info* info) {
	(void)server;
	if(g_component_watch && info->x == g_component_x
	   && info->y == g_component_y && info->z == g_component_z) {
		g_component_notifies++;
		g_component_origin = server_world_notify_origin();
	}
}

static void replace_added_with_dirt(
	struct server_local* server, struct block_info* info) {
	assert(server_world_set_block_silent(
		&server->worlds[0], info->x, info->y, info->z,
		(struct block_data) {.type = BLOCK_DIRT, .metadata = 2}));
}

static void watch_component(int x, int y, int z) {
	g_component_x = x;
	g_component_y = y;
	g_component_z = z;
	g_component_notifies = 0;
	g_component_origin = 0;
	g_component_watch = true;
}

static void assert_component_notified_by(uint8_t origin) {
	g_component_watch = false;
	assert(g_component_notifies > 0);
	assert(g_component_origin == origin);
}

static void test_block_transition_lifecycle(void) {
	const int x = 18, y = 106, z = 18;
	set(x, y, z, BLOCK_BEDROCK, 3);
	void (*saved_bedrock_removal)(struct server_local*, struct block_info*)
		= block_bedrock.onBlockRemoval;
	void (*saved_stone_removal)(struct server_local*, struct block_info*)
		= block_stone.onBlockRemoval;
	void (*saved_stone_added)(struct server_local*, struct block_info*)
		= block_stone.onBlockAdded;
	void (*saved_bedrock_neighbour)(struct server_local*, struct block_info*)
		= block_bedrock.onNeighbourBlockChange;
	block_bedrock.onBlockRemoval = record_block_removal;
	block_stone.onBlockRemoval = record_block_removal;
	block_stone.onBlockAdded = record_block_added;
	block_bedrock.onNeighbourBlockChange = record_component_notify;

	g_lifecycle_stage = 0;
	g_lifecycle_old_type = BLOCK_BEDROCK;
	g_lifecycle_old_meta = 3;
	g_lifecycle_new_type = BLOCK_STONE;
	g_lifecycle_visible_meta = 3;
	g_lifecycle_added_meta = 7;
	set(x, y, z, BLOCK_STONE, 7);
	assert(g_lifecycle_stage == 2);

	/* Identical id+metadata makes Chunk return false, so World does not run
	 * lifecycle, lighting, client update, or neighbour notification. */
	set(x + 1, y, z, BLOCK_BEDROCK, 0);
	watch_component(x + 1, y, z);
	assert(!server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = BLOCK_STONE, .metadata = 7}));
	assert(g_lifecycle_stage == 2);
	g_component_watch = false;
	assert(g_component_notifies == 0);

	/* A metadata difference routed through setBlockIDWithMetadata still performs
	 * removal/addition. Component metadata toggles therefore use metadata-only
	 * primitives; the client-only silent form stays quiet, while the dedicated
	 * WithNotify form is pinned separately below. */
	g_lifecycle_stage = 0;
	g_lifecycle_old_type = BLOCK_STONE;
	g_lifecycle_old_meta = 7;
	g_lifecycle_new_type = BLOCK_STONE;
	g_lifecycle_visible_meta = 7;
	g_lifecycle_added_meta = 8;
	set(x, y, z, BLOCK_STONE, 8);
	assert(g_lifecycle_stage == 2);

	g_lifecycle_stage = 0;
	assert(server_world_set_block_silent(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = BLOCK_STONE, .metadata = 9}));
	assert(g_lifecycle_stage == 0);
	assert(!server_world_set_block_silent(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = BLOCK_STONE, .metadata = 9}));

	/* World.setBlockMetadata is a distinct primitive: it writes/marks even for
	 * the same nibble and never dispatches removal/addition. Its default
	 * WithNotify wrapper must still publish + notify that identical value. This
	 * is the exact piston extension latch/tail sequence. */
	g_lifecycle_stage = 0;
	watch_component(x + 1, y, z);
	assert(server_world_set_block_metadata_quiet(&s.worlds[0], x, y, z, 10));
	assert(g_lifecycle_stage == 0 && g_component_notifies == 0);
	struct block_data metadata_live;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &metadata_live));
	assert(metadata_live.type == BLOCK_STONE && metadata_live.metadata == 10);
	assert(server_world_set_block_metadata_with_notify(&s.worlds[0], x, y, z, 10));
	assert(g_lifecycle_stage == 0);
	assert_component_notified_by(BLOCK_STONE);

	block_bedrock.onBlockRemoval = saved_bedrock_removal;
	block_bedrock.onNeighbourBlockChange = saved_bedrock_neighbour;
	block_stone.onBlockRemoval = saved_stone_removal;
	block_stone.onBlockAdded = saved_stone_added;
	puts("  lifecycle: block writes + quiet/identical metadata-notify primitives OK");
}

static const struct server_chunk* g_quiet_chunk;
static size_t g_quiet_index;
static unsigned g_quiet_metadata_stores, g_quiet_requests;
static unsigned g_quiet_published;

static void record_quiet_rpc(const struct client_rpc* call, void* user) {
	(void)user;
	if(call->type == CRPC_SET_BLOCK) {
		assert(call->payload.set_block.x == 18 && call->payload.set_block.y == 106
			&& call->payload.set_block.z == 18);
		assert(g_lifecycle_stage == 2); /* on-added precedes the client mark */
		g_quiet_published++;
	}
}

static bool record_quiet_store(const struct server_world_history_event* e) {
	if(!strcmp(e->operation, "store") && e->y == COLUMN_META
	   && e->chunk == g_quiet_chunk && (size_t)e->x == g_quiet_index) {
		assert(g_lifecycle_stage == 1); /* removal already ran */
		if(g_quiet_metadata_stores == 0)
			assert(g_quiet_requests == 0);
		else
			assert(g_quiet_metadata_stores == 1 && g_quiet_requests >= 2);
		g_quiet_metadata_stores++;
	}
	return true;
}

static bool record_quiet_order(const struct lighting_history_event* e) {
	if(!strcmp(e->operation, "region-request")) {
		assert(g_quiet_metadata_stores == 1);
		g_quiet_requests++;
	}
	return true;
}

static void record_quiet_added(struct server_local* server, struct block_info* info) {
	assert(g_quiet_metadata_stores == 2 && g_quiet_requests >= 2);
	record_block_added(server, info);
}

static void copy_quiet_planes(const struct server_chunk* sc, uint8_t* out) {
	memcpy(out, sc->ids, SERVER_CHUNK_IDS_BYTES);
	out += SERVER_CHUNK_IDS_BYTES;
	memcpy(out, sc->metadata, SERVER_CHUNK_NIBBLE_BYTES);
	out += SERVER_CHUNK_NIBBLE_BYTES;
	memcpy(out, sc->lighting_sky, SERVER_CHUNK_NIBBLE_BYTES);
	out += SERVER_CHUNK_NIBBLE_BYTES;
	memcpy(out, sc->lighting_torch, SERVER_CHUNK_NIBBLE_BYTES);
	out += SERVER_CHUNK_NIBBLE_BYTES;
	memcpy(out, sc->heightmap, SERVER_CHUNK_HEIGHTMAP_BYTES);
}

/* RetroMCP server Chunk.java:219-249, World.java:192-205,244-271:
 * same ID/different metadata through the ID API lights and runs lifecycle;
 * identity and metadata-only operations have their distinct quiet semantics. */
static void test_quiet_id_metadata_lifecycle(void) {
	const int x = 18, y = 106, z = 18;
	struct server_world* w = &s.worlds[0];
	assert(server_world_light_drain(w));
	set(x + 1, y, z, BLOCK_BEDROCK, 0);
	assert(server_world_light_drain(w));
	void (*removal)(struct server_local*, struct block_info*) = block_stone.onBlockRemoval;
	void (*added)(struct server_local*, struct block_info*) = block_stone.onBlockAdded;
	void (*neighbour)(struct server_local*, struct block_info*) = block_bedrock.onNeighbourBlockChange;
	block_stone.onBlockRemoval = record_block_removal;
	block_stone.onBlockAdded = record_quiet_added;
	block_bedrock.onNeighbourBlockChange = record_component_notify;
	struct block_data old;
	assert(server_world_get_block(w, x, y, z, &old) && old.type == BLOCK_STONE);
	g_quiet_chunk = dict_server_chunks_get(w->chunks, S_CHUNK_ID(1, 1));
	assert(g_quiet_chunk);
	g_quiet_index = y + (z % CHUNK_SIZE + (x % CHUNK_SIZE) * CHUNK_SIZE) * WORLD_HEIGHT;
	g_lifecycle_stage = 0;
	g_lifecycle_old_type = g_lifecycle_new_type = BLOCK_STONE;
	g_lifecycle_old_meta = g_lifecycle_visible_meta = old.metadata;
	g_lifecycle_added_meta = (old.metadata + 1) & 15;
	g_quiet_metadata_stores = g_quiet_requests = 0;
	g_quiet_published = 0;
	clin_test_set_rpc_observer(record_quiet_rpc, NULL);
	struct random_gen rng = s.rand_src;
	watch_component(x + 1, y, z);
	lighting_test_history_reset();
	server_world_test_history_reset();
	server_world_test_history_hook = record_quiet_store;
	lighting_test_history_hook = record_quiet_order;
	assert(server_world_set_block_quiet(w, x, y, z,
		(struct block_data){.type = BLOCK_STONE, .metadata = g_lifecycle_added_meta}));
	lighting_test_history_hook = NULL;
	server_world_test_history_hook = NULL;
	assert(!lighting_test_history_failed());
	assert(!server_world_test_history_failed());
	assert(g_lifecycle_stage == 2 && g_component_notifies == 0);
	assert(g_quiet_published == 1);
	assert(w->generation_light_n > 0 && w->light_seed_n == 0);
	assert(!memcmp(&rng, &s.rand_src, sizeof rng));
	assert(server_world_light_drain(w));
	const size_t bytes = SERVER_CHUNK_IDS_BYTES + 3 * SERVER_CHUNK_NIBBLE_BYTES + SERVER_CHUNK_HEIGHTMAP_BYTES;
	uint8_t* snapshot = malloc(2 * bytes);
	assert(snapshot);
	copy_quiet_planes(g_quiet_chunk, snapshot);
	bool modified = g_quiet_chunk->modified;
	/* Identity rejects BEFORE reserve, callbacks, or any write. */
	server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, 0);
	assert(!server_world_set_block_quiet(w, x, y, z,
		(struct block_data){.type = BLOCK_STONE, .metadata = g_lifecycle_added_meta}));
	assert(server_world_test_light_queue_fault_count(SERVER_WORLD_TEST_LQ_RESERVE) == 0);
	assert(!server_world_light_halted(w));
	copy_quiet_planes(g_quiet_chunk, snapshot + bytes);
	assert(!memcmp(snapshot, snapshot + bytes, bytes));
	assert(g_quiet_chunk->modified == modified && g_lifecycle_stage == 2);
	assert(g_quiet_published == 1);
	/* Preflight failure is atomic for all five planes and lifecycle. */
	assert(!server_world_set_block_quiet(w, x, y, z,
		(struct block_data){.type = BLOCK_STONE, .metadata = (g_lifecycle_added_meta + 1) & 15}));
	assert(w->light_fault && w->light_fault_reason == SERVER_LIGHT_FAULT_RESERVE);
	copy_quiet_planes(g_quiet_chunk, snapshot + bytes);
	assert(!memcmp(snapshot, snapshot + bytes, bytes));
	assert(g_quiet_chunk->modified == modified && g_lifecycle_stage == 2);
	assert(g_quiet_published == 1);
	assert(!server_world_light_pending(w));
	server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, -1);
	server_world_test_light_clear_fault(w);
	clin_server_publication_stop(false);
	/* Later failure retains an admitted Sky request; removal may have run,
	 * but on-added/publication may not. No arbitrary callback rollback claim. */
	g_lifecycle_stage = 0;
	g_lifecycle_old_meta = g_lifecycle_visible_meta = g_lifecycle_added_meta;
	g_lifecycle_added_meta = (g_lifecycle_added_meta + 1) & 15;
	server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, 2);
	assert(!server_world_set_block_quiet(w, x, y, z,
		(struct block_data){.type = BLOCK_STONE, .metadata = g_lifecycle_added_meta}));
	assert(w->light_fault && w->light_fault_reason == SERVER_LIGHT_FAULT_RESERVE);
	assert(g_lifecycle_stage == 1 && w->generation_light_n > 0);
	assert(g_quiet_published == 1);
	assert(g_component_notifies == 0);
	server_world_test_light_queue_fault(SERVER_WORLD_TEST_LQ_RESERVE, -1);
	server_world_test_light_clear_fault(w);
	clin_server_publication_stop(false);
	clin_test_set_rpc_observer(NULL, NULL);
	block_stone.onBlockRemoval = removal;
	block_stone.onBlockAdded = added;
	block_bedrock.onNeighbourBlockChange = neighbour;
	g_component_watch = false;
	assert(server_world_light_drain(w));
	free(snapshot);
	puts("IN-005: quiet lifecycle/order, identity, preflight and post-mutation containment PASS");
}

static void test_power_source_removal_lifecycle(void) {
	void (*saved)(struct server_local*, struct block_info*)
		= block_bedrock.onNeighbourBlockChange;
	block_bedrock.onNeighbourBlockChange = record_component_notify;

	/* The watched cell is two blocks away: ordinary set-air notification cannot
	 * reach it. Only each powered component's mount/support removal callback can. */
	const int y = 108, z = 20;
	set(12, y, z, BLOCK_BEDROCK, 0);
	set(13, y, z, BLOCK_STONE, 0);
	set(14, y, z, BLOCK_LEVER, 1 | 8);
	watch_component(12, y, z);
	set(14, y, z, BLOCK_AIR, 0);
	assert_component_notified_by(BLOCK_LEVER);

	set(12, y + 2, z, BLOCK_BEDROCK, 0);
	set(13, y + 2, z, BLOCK_STONE, 0);
	set(14, y + 2, z, BLOCK_STONE_BUTTON, 1 | 8);
	watch_component(12, y + 2, z);
	set(14, y + 2, z, BLOCK_AIR, 0);
	assert_component_notified_by(BLOCK_STONE_BUTTON);

	set(18, y - 2, z, BLOCK_BEDROCK, 0);
	set(18, y - 1, z, BLOCK_STONE, 0);
	set(18, y, z, BLOCK_STONE_PRESSURE_PLATE, 1);
	watch_component(18, y - 2, z);
	set(18, y, z, BLOCK_AIR, 0);
	assert_component_notified_by(BLOCK_STONE_PRESSURE_PLATE);

	set(22, y, z, BLOCK_BEDROCK, 0);
	set(23, y, z, BLOCK_STONE, 0); /* meta 1 torch mount (-X) */
	set(24, y, z, BLOCK_REDSTONE_TORCH_LIT, 1);
	watch_component(22, y, z);
	set(24, y, z, BLOCK_AIR, 0);
	assert_component_notified_by(BLOCK_REDSTONE_TORCH_LIT);

	/* Sloped dust is not a face-neighbour of the removed lower wire. Its level
	 * must nevertheless fall immediately through the exact wire-removal reach. */
	set(27, y, z, BLOCK_STONE, 0);
	assert(server_world_set_block_silent(
		&s.worlds[0], 26, y, z,
		(struct block_data) {.type = BLOCK_REDSTONE_WIRE, .metadata = 15}));
	assert(server_world_set_block_silent(
		&s.worlds[0], 27, y + 1, z,
		(struct block_data) {.type = BLOCK_REDSTONE_WIRE, .metadata = 14}));
	set(26, y, z, BLOCK_AIR, 0);
	assert(gmeta(27, y + 1, z) == 0);

	block_bedrock.onNeighbourBlockChange = saved;
	puts("  removal: lever/button/plate/lit-torch mount depower + sloped wire flood OK");
}

static void test_nested_added_notify_origin(void) {
	const int x = 20, y = 104, z = 20;
	/* This cell is fresh air. Identical setBlockAndMetadataWithNotify is now
	 * correctly a no-op, so assert the fixture instead of demanding a write. */
	assert(gid(x, y, z) == BLOCK_AIR && gmeta(x, y, z) == 0);
	set(x + 1, y, z, BLOCK_BEDROCK, 0);
	void (*saved_added)(struct server_local*, struct block_info*)
		= block_stone.onBlockAdded;
	void (*saved_notify)(struct server_local*, struct block_info*)
		= block_bedrock.onNeighbourBlockChange;
	block_stone.onBlockAdded = replace_added_with_dirt;
	block_bedrock.onNeighbourBlockChange = record_component_notify;
	watch_component(x + 1, y, z);
	set(x, y, z, BLOCK_STONE, 0);
	assert_component_notified_by(BLOCK_STONE);
	assert(gid(x, y, z) == BLOCK_DIRT);
	assert(gmeta(x, y, z) == 2);
	/* Quiet ID+metadata has the same live-publication rule, without generic
	 * neighbour notification. Chunk.java:245-249; the client mark is the
	 * fluid adapter's World.markBlockNeedsUpdate (BlockFlowing.java:18). */
	watch_component(x + 1, y, z);
	struct sleep_rpc_capture quiet = {0};
	clin_test_set_rpc_observer(capture_sleep_rpcs, &quiet);
	assert(server_world_set_block_quiet(&s.worlds[0], x, y, z,
		(struct block_data){.type = BLOCK_STONE, .metadata = 3}));
	clin_test_set_rpc_observer(NULL, NULL);
	assert(quiet.count == 2); /* one nested raw replacement + outer live mark */
	assert(quiet.calls[1].type == CRPC_SET_BLOCK);
	assert(quiet.calls[1].payload.set_block.block.type == BLOCK_DIRT);
	assert(quiet.calls[1].payload.set_block.block.metadata == 2);
	assert(g_component_notifies == 0);
	g_component_watch = false;
	block_stone.onBlockAdded = saved_added;
	block_bedrock.onNeighbourBlockChange = saved_notify;
	puts("  lifecycle: nested onBlockAdded replacement renders live but notifies requested ID OK");
}

static void assert_beta_notify_order(int x, int y, int z) {
	static const int expected[6][3] = {
		{-1, 0, 0}, {1, 0, 0}, {0, -1, 0},
		{0, 1, 0},  {0, 0, -1}, {0, 0, 1},
	};
	assert(g_notify_order_count == 6);
	for(size_t i = 0; i < 6; i++) {
		assert(g_notify_order[i][0] == x + expected[i][0]);
		assert(g_notify_order[i][1] == y + expected[i][1]);
		assert(g_notify_order[i][2] == z + expected[i][2]);
	}
}

static void test_beta_notify_order(void) {
	const int x = 20, y = 100, z = 20;
	static const int offsets[6][3] = {
		{-1, 0, 0}, {1, 0, 0}, {0, -1, 0},
		{0, 1, 0},  {0, 0, -1}, {0, 0, 1},
	};
	set(x, y, z, BLOCK_STONE, 0);
	for(size_t i = 0; i < 6; i++)
		set(x + offsets[i][0], y + offsets[i][1], z + offsets[i][2],
			BLOCK_BEDROCK, 0);
	void (*saved)(struct server_local*, struct block_info*)
		= block_bedrock.onNeighbourBlockChange;
	block_bedrock.onNeighbourBlockChange = record_notify_order;

	g_notify_order_count = 0;
	notifyNeighbours_from(&s, x, y, z, BLOCK_STONE);
	assert_beta_notify_order(x, y, z);

	g_notify_order_count = 0;
	set(x, y, z, BLOCK_DIRT, 0);
	assert_beta_notify_order(x, y, z);
	block_bedrock.onNeighbourBlockChange = saved;
	puts("  neighbours: explicit and generic dispatch order -X,+X,-Y,+Y,-Z,+Z OK");
}
static int gid(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? b.type : -1;
}
static int gmeta(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.worlds[0], x, y, z, &b) ? (b.metadata & 0x0F) : -1;
}
static void cake_click(int x, int y, int z) {
	struct block_data cd;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &cd));
	struct block_info on = {.block = &cd, .neighbours = NULL, .x = x, .y = y, .z = z};
	blocks[BLOCK_CAKE]->onRightClick(&s, &s.player, NULL, NULL, &on, SIDE_TOP);
}
static void farmland_tick(int x, int y, int z) {
	struct block_data fd;
	assert(server_world_get_block(&s.worlds[0], x, y, z, &fd));
	struct block_info info = {.block = &fd, .neighbours = NULL, .x = x, .y = y, .z = z};
	blocks[BLOCK_FARMLAND]->onRandomTick(&s, &info);
}
/* Exercise the actual ordered crop override without publishing entities. */
static bool crop_count_sink(void* ctx, const struct server_local_planned_drop* drop) {
	int* counts = ctx;
	if(drop->item.id == ITEM_SEED) counts[0]++;
	if(drop->item.id == ITEM_WHEAT) counts[1]++;
	struct server_local_planned_drop owned = *drop;
	server_local_cancel_planned_drop(&owned);
	return true;
}
static void crop_drop_trial(uint8_t meta, int* out_seeds, int* out_wheat) {
	struct block_data bd = {.type = BLOCK_CROPS, .metadata = meta};
	struct block_info info = {.block = &bd, .x = 0, .y = Y, .z = 0};
	int counts[2] = {0};
	assert(server_local_plan_block_drops_w(&s.worlds[0], &info, crop_count_sink, counts));
	assert(counts[0] >= 0 && counts[0] <= 3);
	*out_seeds = counts[0];
	*out_wheat = counts[1];
}

int main(void) {
	test_fixture_dir("test_block_behavior_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	clin_init();
	/* Headless tests deliberately do not create a GLFW window. All physical
	 * input paths must therefore report neutral input without calling GLFW. */
	float headless_x = 1.0F, headless_y = 1.0F, headless_angle = 1.0F;
	assert(!input_pointer(&headless_x, &headless_y, &headless_angle));
	assert(headless_x == 0.0F && headless_y == 0.0F
		   && headless_angle == 0.0F);
	headless_x = headless_y = 1.0F;
	assert(input_joystick(1.0F / 60.0F, &headless_x, &headless_y));
	assert(headless_x == 0.0F && headless_y == 0.0F);
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init(); // food eat/maxstack assertions read the item registry

	/* MOB-SPAWN CELL GEOMETRY [740c583 SpawnerAnimals.canCreatureTypeSpawnAtLocation].
	 * Beta needs isBlockNormalCube(y-1) && !isBlockNormalCube(y) && !liquid(y)
	 * && !isBlockNormalCube(y+1). In this port "opaque full cube" is
	 * `!can_see_through && !transparent` -- and that is the ONLY correct source
	 * for it. server_local.c's mob_solid_floor used to test `opacity >= 15`
	 * instead, but `opacity` is a designated-initializer field that the plain
	 * solid blocks NEVER SET (block_stone.c / block_grass.c / block_dirt.c have
	 * no `.opacity` line, so it defaults to 0). The predicate was therefore
	 * FALSE FOR STONE, GRASS AND DIRT and no cell in the world could pass the
	 * floor test: measured on the maintainer's save, 6162 of 6162 candidates
	 * rejected, the light gate never once reached, zero hostile/passive/water
	 * spawns in 30 minutes. These asserts inverting is the fail-before. */
	{
		/* Call the PRODUCTION predicate, not a restatement of it. mob_solid_floor
		 * in server_local.c is `return block_is_normal_cube(type);`, so asserting
		 * on block_is_normal_cube over the REAL registry is asserting on the code
		 * that ships. The first version of this block only read blocks[] fields
		 * (`opacity < 15` etc.), which is true both before and after the fix --
		 * it guarded nothing, and adversarial review caught the bogus
		 * "fail-before" claim. Reverting mob_solid_floor to the old
		 * `opacity >= 15` now fails here. */
		const uint8_t floors[] = {BLOCK_STONE, BLOCK_GRASS, BLOCK_DIRT,
								  BLOCK_COBBLESTONE};
		for(size_t i = 0; i < sizeof(floors) / sizeof(*floors); i++) {
			assert(blocks[floors[i]]
				   && "a mob-standable block must exist in the registry");
			assert(block_is_normal_cube(floors[i])
				   && "stone/grass/dirt/cobblestone MUST read as opaque full "
					  "cubes -- a mob has to be able to stand on them, and "
					  "cobblestone is every Beta dungeon floor");
			assert(blocks[floors[i]]->opacity < 15
				   && "these blocks leave .opacity at its 0 default -- which is "
					  "why a spawn predicate must NEVER gate on opacity>=15");
		}
		/* A snow LAYER is not a full cube: Beta lets a mob stand IN it, with the
		 * grass beneath as the floor. Requiring literal AIR for the body cell
		 * (the other half of the fix) made every snow-covered surface -- i.e.
		 * the maintainer's entire world -- unspawnable. */
		assert(blocks[BLOCK_SNOW] && !block_is_normal_cube(BLOCK_SNOW)
			   && "a snow layer must NOT read as a full cube (mobs stand in it)");

		/* ...and it must have NO COLLISION BOX at metadata < 3, or a mob that
		 * spawns standing in it is wedged forever. Beta
		 * [740c583 BlockSnow.getCollisionBoundingBoxFromPool]:
		 *     return var5 >= 3 ? boundingBox(..., y + 0.5F, ...) : null;
		 * getBoundingBox1 ignored its `entity` (collision-query) flag and always
		 * returned a box, so a mob spawned on snow intersected at spawn;
		 * entity_intersection_threshold returns 0 for an already-intersecting
		 * entity, so every axis move became a no-op and the mob froze in place
		 * with its AI still running (2026-07-31 console photos). The one-shot
		 * +0.01 unstuck nudge is smaller than the 0.125 layer, so nothing
		 * recovered it. Reverting the fix makes these two asserts fail. */
		{
			struct block_data snow_thin = {.type = BLOCK_SNOW, .metadata = 0};
			struct block_info si = {.block = &snow_thin, .x = 0, .y = Y, .z = 0};
			assert(blocks[BLOCK_SNOW]->getBoundingBox(&si, true, NULL) == 0
				   && "a thin snow layer must have NO collision box (Beta: null)");

			struct block_data snow_thick = {.type = BLOCK_SNOW, .metadata = 4};
			struct block_info ti = {.block = &snow_thick, .x = 0, .y = Y, .z = 0};
			struct AABB tb;
			assert(blocks[BLOCK_SNOW]->getBoundingBox(&ti, true, &tb) == 1
				   && "metadata >= 3 snow DOES collide");
			assert(tb.y2 == 0.5F
				   && "Beta collides thick snow at a FIXED y+0.5, not its visual height");

			/* The selection/render box is unchanged: still the visual height. */
			struct AABB vb;
			assert(blocks[BLOCK_SNOW]->getBoundingBox(&si, false, &vb) == 1
				   && vb.y2 == 0.125F
				   && "the non-collision box must still be the visual height");
		}
	}
	{
		/* FH-15: redstone wire selection-box height. Beta
		 * [740c583 BlockRedstoneWire.java:14]:
		 *     this.setBlockBounds(0.0F, 0.0F, 0.0F, 1.0F, 0.0625F, 1.0F);
		 * The fork family inherited 0.125F (2x Beta's dust height). */
		struct block_data wd = {.type = BLOCK_REDSTONE_WIRE, .metadata = 0};
		struct block_info wi = {.block = &wd, .x = 0, .y = Y, .z = 0};
		struct AABB wb;
		assert(blocks[BLOCK_REDSTONE_WIRE]->getBoundingBox(&wi, false, &wb) == 1
			   && wb.y2 - wb.y1 == 0.0625F
			   && "Beta wire dust box is 0.0625F tall, not 0.125F");
	}
	{
		/* FH-02: leaf-decay sapling odds are nextInt(20)==0, not the
		 * fork-inherited %16. Beta [740c583 BlockLeaves.java:155-157]:
		 *     one drop when nextInt(20) is 0, else none;
		 * Golden: drop count over 20000 draws of a FIXED seed (expected mean
		 * 1000). Reverting to 1/16 (mean 1250) lands far outside the pin.
		 * F4-RNG: the stream is now java.util.Random; 980 is the JVM witness
		 * (tmp/jobs/lead-1010/f4rng-witness/F4LeafWitness.java on OpenJDK
		 * 21.0.7: Random seeded 20260821L|1L, nextInt(20)==0 x20000; source
		 * sha256 75a3ca41..., output sha256 ab8fe7f1...). The xorshift-era
		 * pin was 981 from the candidate itself. */
		struct java_random lg;
		jrand_set_seed(&lg, 20260821 | 1);
		struct block_data ld = {.type = BLOCK_LEAVES, .metadata = 0};
		struct block_info li = {.block = &ld, .x = 0, .y = Y, .z = 0};
		int drops = 0;
		for(int i = 0; i < 20000; i++)
			drops += block_quantity_dropped(BLOCK_LEAVES, &lg);
		assert(drops == 980 && "leaf decay must drop saplings at Beta's 1/20");
	}
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.worlds[0], level, WORLD_DIM_OVERWORLD, 4242, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	// Fixed nonzero seed (golden-ratio constant) instead of rand_gen_seed's time(NULL): the
	// distribution sections (trample ~1/4, crops seed counts) don't need wall-clock entropy, a
	// fixed seed makes any failure deterministically reproducible, and it sidesteps the xorshift
	// seed==0 degenerate fixed point (a 0 seed yields an all-zero stream -> every roll 0).
	s.rand_src.seed = 0x9E3779B9u;
	dict_entity_init(s.entities); // reed removal spawns item-drop entities into this dict
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.worlds[0], cx, cz, &sc) && sc);
	test_block_transition_lifecycle();
	test_quiet_id_metadata_lifecycle();
	test_power_source_removal_lifecycle();
	test_nested_added_notify_origin();
	test_beta_notify_order();

	// ===================== REED water-adjacency (reed_can_block_stay) =====================
	{
		const int X = 4, Z = 4;
		// dirt base, NO water around -> a reed on it cannot stay
		set(X, Y, Z, BLOCK_DIRT, 0);
		assert(!reed_can_block_stay(&s, X, Y + 1, Z));
		// add water beside the base -> now it can stay
		set(X + 1, Y, Z, BLOCK_WATER_STILL, 0);
		assert(reed_can_block_stay(&s, X, Y + 1, Z));
		// flowing water also counts
		set(X + 1, Y, Z, BLOCK_AIR, 0);
		set(X, Y, Z - 1, BLOCK_WATER_FLOW, 0);
		assert(reed_can_block_stay(&s, X, Y + 1, Z));
		// reed-on-reed: always stays (no water needed)
		set(X, Y + 1, Z, BLOCK_REED, 0);
		assert(reed_can_block_stay(&s, X, Y + 2, Z));
		// on stone (non-soil): never stays
		set(X + 4, Y, Z, BLOCK_STONE, 0);
		set(X + 4, Y + 1, Z, BLOCK_WATER_STILL, 0); // water present but base is stone
		assert(!reed_can_block_stay(&s, X + 4, Y + 1, Z));
		// remove the water under a planted reed -> onNeighbourBlockChange drops it
		set(X + 8, Y, Z, BLOCK_DIRT, 0);
		set(X + 9, Y, Z, BLOCK_WATER_STILL, 0);
		set(X + 8, Y + 1, Z, BLOCK_REED, 0);
		assert(reed_can_block_stay(&s, X + 8, Y + 1, Z));
		set(X + 9, Y, Z, BLOCK_AIR, 0); // cut the water
		struct block_data rd;
		server_world_get_block(&s.worlds[0], X + 8, Y + 1, Z, &rd);
		struct block_info ri = {.block = &rd, .x = X + 8, .y = Y + 1, .z = Z};
		blocks[BLOCK_REED]->onNeighbourBlockChange(&s, &ri);
		assert(gid(X + 8, Y + 1, Z) == BLOCK_AIR); // dropped
		printf("  reed: water-adjacency stay rule + neighbour-change drop OK\n");
	}

	// ===================== CAKE eating =====================
	{
		const int X = 2, Z = 12;
		set(X, Y, Z, BLOCK_CAKE, 0);
		s.player.health = 8; // wounded, low enough to take all 6 bites without hitting the max-health gate
		int prev = s.player.health;
		for(int bite = 1; bite <= 5; bite++) {
			cake_click(X, Y, Z);
			assert(gid(X, Y, Z) == BLOCK_CAKE);
			assert(gmeta(X, Y, Z) == bite);			   // metadata advances 1..5
			assert(s.player.health == prev + 24);	   // heals 24/bite (Beta 3 * 8)
			prev = s.player.health;
		}
		cake_click(X, Y, Z);					  // the 6th bite removes the cake
		assert(gid(X, Y, Z) == BLOCK_AIR);
		assert(s.player.health == prev + 24);

		// at FULL health the click is a no-op (Beta `health < 20` gate)
		set(X, Y, Z, BLOCK_CAKE, 0);
		s.player.health = MAX_PLAYER_HEALTH;
		cake_click(X, Y, Z);
		assert(gid(X, Y, Z) == BLOCK_CAKE && gmeta(X, Y, Z) == 0); // untouched
		assert(s.player.health == MAX_PLAYER_HEALTH);
		printf("  cake: 6 bites advance + remove, heal 24/bite, full-health no-op OK\n");
	}

	// ===================== FARMLAND hydration =====================
	{
		// hydrate: dry farmland with water within range -> reaches moisture 7
		const int XH = 2, ZH = 20;
		set(XH, Y, ZH, BLOCK_FARMLAND, 0);
		set(XH + 2, Y, ZH, BLOCK_WATER_STILL, 0); // within the 9x9x2 box
		for(int k = 0; k < 400 && gmeta(XH, Y, ZH) != 7; k++)
			farmland_tick(XH, Y, ZH);
		assert(gmeta(XH, Y, ZH) == 7); // hydrated

		// dry out: farmland with NO water and no crop above -> eventually reverts to dirt. Keep it well
		// clear (>8) of the hydrate scenario's water so its own 9x9 scan stays dry.
		const int XD = 24, ZD = 26;
		set(XD, Y, ZD, BLOCK_FARMLAND, 5); // start partially moist, no water nearby
		for(int k = 0; k < 2000 && gid(XD, Y, ZD) == BLOCK_FARMLAND; k++)
			farmland_tick(XD, Y, ZD);
		assert(gid(XD, Y, ZD) == BLOCK_DIRT); // dried all the way out -> dirt
		printf("  farmland: hydrate-to-7 near water + dry-out-to-dirt OK\n");
	}

	// ===================== FARMLAND TRAMPLE (Beta onEntityWalking + footstep cadence) =====================
	{
		// (a) Footstep cadence (server_player_walk_step): distanceWalked accumulates +sqrt(dx^2+dz^2)*0.6
		// per update and ONE footstep fires each time it crosses the integer next_step_distance, only with
		// a SOLID block underfoot. +0.3/call (dx=0.5) crosses 1 at call 4 (dw 1.2) and 2 at call 7 (dw 2.1)
		// -> 2 steps over 9 calls; crossings sit 0.2 off the integers, so the count is float-robust.
		float dw = 0.0f;
		int nsd = 1, steps = 0;
		for(int i = 0; i < 9; i++)
			steps += server_player_walk_step(&dw, &nsd, 0.5, 0.0, true) ? 1 : 0;
		assert(steps == 2 && nsd == 3);
		// at most ONE footstep per update even on a big jump (Beta fires an `if`, not a `while`):
		dw = 0.0f; nsd = 1; steps = 0;
		for(int i = 0; i < 3; i++)
			steps += server_player_walk_step(&dw, &nsd, 8.4, 0.0, true) ? 1 : 0; // dx 8.4 -> +5.04/call
		assert(steps == 3 && nsd == 4);
		// airborne (no solid block underfoot): distance still accumulates, but no step fires until the
		// entity lands again -- Beta gates both the step and the ++ on var28 > 0.
		dw = 0.0f; nsd = 1;
		assert(!server_player_walk_step(&dw, &nsd, 2.0, 0.0, false)); // dw 1.2 > 1 but airborne
		assert(nsd == 1 && dw > 1.0f);
		assert(server_player_walk_step(&dw, &nsd, 0.0, 0.0, true)); // lands -> the pending step fires
		assert(nsd == 2);

		// (b) Trample probability: block_farmland_on_entity_walking reverts farmland -> dirt on a 1-in-4
		// roll. Over many trials (re-laying the farmland after each revert) the rate is ~1/4 (rand_gen is
		// distribution-faithful, not byte-exact to Beta's nextInt) -- a wide band still catches 1/2 or 0.
		const int TX = 10, TZ = 8, TRIALS = 4000; // within the loaded 2x2 chunk area [0,32)
		int reverts = 0;
		for(int i = 0; i < TRIALS; i++) {
			set(TX, Y, TZ, BLOCK_FARMLAND, 0);
			block_farmland_on_entity_walking(&s, TX, Y, TZ);
			if(gid(TX, Y, TZ) == BLOCK_DIRT)
				reverts++;
		}
		assert(reverts > TRIALS / 6 && reverts < TRIALS / 3); // ~1000; clearly 1/4, not 1/2 or 1/8
		printf("  trample: cadence (2 steps/9, 1/jump, airborne-gated) + ~1/4 revert (%d/%d) OK\n",
			   reverts, TRIALS);

		// (c) A crop pops when its farmland support is removed (Beta canBlockStay via neighbour-notify),
		// whether the farmland is MINED or TRAMPLED to dirt.
		set(TX, Y, TZ, BLOCK_FARMLAND, 0);
		set(TX, Y + 1, TZ, BLOCK_CROPS, 3);
		assert(gid(TX, Y + 1, TZ) == BLOCK_CROPS); // sits happily on farmland
		set(TX, Y, TZ, BLOCK_AIR, 0);			   // mine the farmland out
		assert(gid(TX, Y + 1, TZ) == BLOCK_AIR);   // crop lost support -> popped
		set(TX, Y, TZ, BLOCK_FARMLAND, 0);
		set(TX, Y + 1, TZ, BLOCK_CROPS, 7);
		for(int i = 0; i < 200 && gid(TX, Y, TZ) == BLOCK_FARMLAND; i++)
			block_farmland_on_entity_walking(&s, TX, Y, TZ); // trample until it reverts
		assert(gid(TX, Y, TZ) == BLOCK_DIRT);	 // trampled to dirt
		assert(gid(TX, Y + 1, TZ) == BLOCK_AIR); // and the crop above popped
		printf("  trample: crop above pops when farmland is mined OR trampled to dirt OK\n");
	}

	// ===================== CROPS drop distribution (Beta BlockCrops.dropBlockAsItemWithChance) =====
	{
		// Beta: 3 independent seed rolls, each drops a seed iff nextInt(15) <= metadata -> seed count is
		// Binomial(3, (metadata+1)/15) with mean (metadata+1)/5, AT EVERY stage; plus 1 wheat iff
		// metadata == 7. rand_gen_int is distribution-faithful (not byte-exact to nextInt), so we pin the
		// per-stage seed MEAN within a band and the crisp wheat rule, not an exact RNG sequence.
		const int TRIALS = 20000;
		int seed_total[8] = {0};	  // seeds summed over TRIALS, per growth stage
		int wheat_total[8] = {0};	  // wheat summed over TRIALS, per growth stage
		int seed_hist7[4] = {0};	  // seed-count histogram (0..3) at full growth (metadata 7)
		for(int meta = 0; meta <= 7; meta++) {
			for(int t = 0; t < TRIALS; t++) {
				int seeds = 0, wheat = 0;
				crop_drop_trial((uint8_t)meta, &seeds, &wheat);
				seed_total[meta] += seeds;
				wheat_total[meta] += wheat;
				if(meta == 7)
					seed_hist7[seeds]++;
			}
		}

		// (a) WHEAT only at full growth: zero below 7, exactly one per trial at 7 (idDropped rule).
		for(int meta = 0; meta < 7; meta++)
			assert(wheat_total[meta] == 0);
		assert(wheat_total[7] == TRIALS); // one wheat every trial at metadata 7

		// (b) SEEDS drop at EVERY stage, including the early ones the OLD code (metadata >= 5 gate) gave
		// nothing -- and the per-stage mean tracks Beta's (metadata+1)/5 within a tight band. The band is
		// far below the old behaviour at every stage (old: 0 seeds for meta<5, flat ~2.0 for meta>=5), so
		// this golden fails on a regression to either.
		double prev_mean = -1.0;
		for(int meta = 0; meta <= 7; meta++) {
			double mean = (double)seed_total[meta] / TRIALS;
			double expect = (meta + 1) / 5.0; // 3 * (meta+1)/15
			assert(seed_total[meta] > 0);				// seeds at EVERY stage (kills the >=5 gate)
			assert(mean > expect - 0.15 && mean < expect + 0.15); // within band of Beta's binomial mean
			assert(mean > prev_mean);					// strictly scales up with growth stage
			prev_mean = mean;
		}

		// (c) At full growth the seed count spans the full Beta 0..3 range -- crucially 0 occurs (the old
		// flat rand_gen_range(1,3) could never drop 0 seeds once metadata >= 5).
		for(int c = 0; c <= 3; c++)
			assert(seed_hist7[c] > 0);

		printf("  crops drop: seed mean/stage [%.2f %.2f %.2f %.2f %.2f %.2f %.2f %.2f] "
			   "(Beta (m+1)/5), wheat only@7=%d, full-growth 0..3 seeds=[%d %d %d %d] OK\n",
			   seed_total[0] / (double)TRIALS, seed_total[1] / (double)TRIALS,
			   seed_total[2] / (double)TRIALS, seed_total[3] / (double)TRIALS,
			   seed_total[4] / (double)TRIALS, seed_total[5] / (double)TRIALS,
			   seed_total[6] / (double)TRIALS, seed_total[7] / (double)TRIALS,
			   wheat_total[7], seed_hist7[0], seed_hist7[1], seed_hist7[2], seed_hist7[3]);
	}

	// ===================== FOOD: eat HEALS (Beta ItemFood), never damages =====================
	{
		struct item_data dummy = {0};
		// Raw porkchop: Beta porkRaw = ItemFood(63,3) heals 3 pts = +24 (8x scale).
		// The bug was a debug stub dealing -16 DAMAGE. Raw fish: ItemFood(93,2) = +16.
		// Cooked fish: ItemFood(94,5) = +40 (was over-healing to 48).
		s.player.health = 100;
		assert(items[ITEM_PORKCHOP]->onItemPlace(&s, &s.player, &dummy, NULL, NULL, SIDE_TOP));
		assert(s.player.health == 124);
		s.player.health = 100;
		assert(items[ITEM_FISH]->onItemPlace(&s, &s.player, &dummy, NULL, NULL, SIDE_TOP));
		assert(s.player.health == 116);
		s.player.health = 100;
		assert(items[ITEM_FISH_COOKED]->onItemPlace(&s, &s.player, &dummy, NULL, NULL, SIDE_TOP));
		assert(s.player.health == 140);
		// At full health: no-op, food not consumed (returns false), health unchanged.
		s.player.health = MAX_PLAYER_HEALTH;
		assert(!items[ITEM_PORKCHOP]->onItemPlace(&s, &s.player, &dummy, NULL, NULL, SIDE_TOP));
		assert(!items[ITEM_FISH]->onItemPlace(&s, &s.player, &dummy, NULL, NULL, SIDE_TOP));
		assert(s.player.health == MAX_PLAYER_HEALTH);
		// Beta ItemFood.maxStackSize == 1 for every food.
		assert(items[ITEM_PORKCHOP]->max_stack == 1);
		assert(items[ITEM_PORKCHOP_COOKED]->max_stack == 1);
		assert(items[ITEM_FISH]->max_stack == 1);
		assert(items[ITEM_FISH_COOKED]->max_stack == 1);
		assert(items[ITEM_BREAD]->max_stack == 1);
		printf("  food: raw pork/fish HEAL +24/+16 (not damage), cooked fish +40, "
			   "full-health no-op, maxstack 1 OK\n");

		// Batch H (2026-08-25): Cookie (357) was registered with a working
		// recipe but ZERO use behavior (right-click did nothing). Beta
		// [decompile 740c583] Item.java:375 `new ItemCookie(101, 1, false,
		// 8)`: healAmount=1 half-heart -> +HEALTH_PER_HEART/2 (8 units),
		// maxStackSize=8 (the ItemCookie ctor's 4th arg OVERRIDES ItemFood's
		// own maxStackSize=1 default -- cookies alone among Beta foods stack
		// past 1).
		s.player.health = 100;
		assert(items[ITEM_COOKIE]->onItemPlace(&s, &s.player, &dummy, NULL, NULL, SIDE_TOP));
		assert(s.player.health == 100 + HEALTH_PER_HEART / 2);
		s.player.health = MAX_PLAYER_HEALTH;
		assert(!items[ITEM_COOKIE]->onItemPlace(&s, &s.player, &dummy, NULL, NULL, SIDE_TOP));
		assert(s.player.health == MAX_PLAYER_HEALTH);
		assert(items[ITEM_COOKIE]->max_stack == 8);
		printf("  cookie: eat HEALS +%d (half heart), full-health no-op, maxstack 8 OK\n",
			   HEALTH_PER_HEART / 2);

		// Batch H (2026-08-25): Bowl (281) was max_stack=1 with no citation.
		// Beta [decompile 740c583] Item.java:299 `bowlEmpty = (new Item(25))
		// .setIconCoord(7,4).setItemName("bowl");` -- plain Item, never
		// calls setMaxStackSize, so it keeps the class default
		// `maxStackSize = 64` (Item.java:115).
		assert(items[ITEM_BOWL]->max_stack == 64);
		printf("  bowl: maxstack 64 (Beta Item default, no setMaxStackSize) OK\n");
	}

	// =============== ORE/GLOWSTONE drop counts (Beta quantityDropped, inclusive ranges) ========
	{
		struct block_data bd = {.type = 0};
		struct block_info bi = {.block = &bd, .x = 0, .y = Y, .z = 0};
		int rmin = 99, rmax = 0, lmin = 99, lmax = 0, gmin = 99, gmax = 0;
		int rsaw5 = 0, lsaw8 = 0, gsaw4 = 0;
		for(int t = 0; t < 5000; t++) {
			struct item_data d = {0};
			block_redstoneore.getDroppedItem(&bi, &d, &s.worlds[0].rand, &s); // identity, followed by quantity below
			d.count = block_quantity_dropped(73, &s.worlds[0].rand);
			rmin = d.count < rmin ? d.count : rmin;
			rmax = d.count > rmax ? d.count : rmax;
			rsaw5 |= (d.count == 5);
			d = (struct item_data) {0};
			block_lapisore.getDroppedItem(&bi, &d, &s.worlds[0].rand, &s); // identity, followed by quantity below
			d.count = block_quantity_dropped(BLOCK_LAPIS_ORE, &s.worlds[0].rand);
			lmin = d.count < lmin ? d.count : lmin;
			lmax = d.count > lmax ? d.count : lmax;
			lsaw8 |= (d.count == 8);
			d = (struct item_data) {0};
			block_glowstone.getDroppedItem(&bi, &d, &s.worlds[0].rand, &s); // identity, followed by quantity below
			d.count = block_quantity_dropped(BLOCK_GLOWSTONE, &s.worlds[0].rand);
			gmin = d.count < gmin ? d.count : gmin;
			gmax = d.count > gmax ? d.count : gmax;
			gsaw4 |= (d.count == 4);
		}
		assert(rmin == 4 && rmax == 5 && rsaw5); // never the old constant 4
		assert(lmin == 4 && lmax == 8 && lsaw8); // top 8 was unreachable before
		assert(gmin == 2 && gmax == 4 && gsaw4); // 4 was unreachable before
		printf("  ore drops: redstone {4,5}, lapis 4..8, glowstone 2..4 (Beta quantityDropped) OK\n");
	}

	// ===================== GRAVEL flint drop (Beta BlockGravel.idDropped nextInt(10)) ==========
	{
		// BUG: getDroppedItem rolled the flint chance with raw `rand_gen(g) %
		// 10 == 0` instead of this project's canonical nextInt(n) port,
		// rand_gen_int(g, n) (see the ORE drops above -- drop_redstone/
		// drop_lapis both use it -- and block_leaves.c's FH-02 fix note).
		// Prove (1) the FIXED gravel code is now byte-for-byte, draw for
		// draw, the SAME as an independently-seeded reference that calls
		// rand_gen_int(g, 10) == 0 directly, and (2) the OLD raw-modulo
		// formula genuinely diverges from that reference on this seed -- so
		// this wasn't a no-op rename. [decompile 740c583 BlockGravel.idDropped]
		struct block_data gbd = {.type = BLOCK_GRAVEL};
		struct block_info gbi = {.block = &gbd, .x = 0, .y = Y, .z = 0};
		/* F4-RNG: World.rand is java.util.Random; the reference is Java
		 * nextInt(10) on an identically seeded stream, and the OLD raw-modulo
		 * formula is evaluated on the same Java stream (next(32) % 10). */
		struct java_random g_gravel;
		jrand_set_seed(&g_gravel, 0x1234ABCD);
		struct java_random g_ref = g_gravel; // same seed, driven independently
		struct java_random g_old = g_gravel; // same seed, drives the OLD formula

		int n = 2000, flints = 0, mismatched_old = 0;
		for(int t = 0; t < n; t++) {
			struct item_data d = {0};
			block_gravel.getDroppedItem(&gbi, &d, &g_gravel, &s);
			bool got_flint = (d.id == ITEM_FLINT);
			bool ref_flint = jrand_next_int_bound(&g_ref, 10) == 0; // the canonical primitive
			assert(got_flint == ref_flint
				   && "gravel's flint roll must be byte-for-byte rand_gen_int(g,10)==0");
			bool old_flint = ((uint32_t)jrand_next_int(&g_old) % 10u) == 0; // the PRE-FIX formula
			if(old_flint != ref_flint)
				mismatched_old++;
			flints += got_flint;
		}
		assert(flints > n / 20 && flints < n / 5); // sanity: ~10% +/- a wide band, not degenerate
		assert(mismatched_old > 0
			   && "the old raw-modulo formula must diverge from rand_gen_int on this seed -- "
				  "otherwise this fix would be a no-op");
		printf("  gravel: flint roll is byte-for-byte rand_gen_int(g,10)==0 (~%.1f%% over %d "
			   "draws); the old raw-modulo formula diverged on %d/%d draws\n",
			   100.0 * flints / n, n, mismatched_old, n);
	}

	// ===================== ICE skylight opacity (Beta setLightOpacity(3)) =====================
	assert(block_ice.opacity == 3); // a lighting VALUE; was wrongly 1
	printf("  ice: lightOpacity == 3 (Beta setLightOpacity(3)) OK\n");

	// ===================== SOUL SAND bounding box (Beta getCollisionBoundingBoxFromPool) ==========
	{
		// Beta BlockSoulSand shrinks ONLY the collision box (entity == true) to height 1 - 0.125 =
		// 0.875, so an entity sinks 1/8 in; the selection/render box (entity == false) stays a full
		// cube (getSelectedBoundingBoxFromPool is not overridden). Regular sand is a full cube for both.
		// 0.875 = 7*2^-3 is exact in float, so == is safe. [decompile 740c583 BlockSoulSand]
		struct block_info bbi = {0};
		struct AABB box;
		assert(block_soulsand.getBoundingBox(&bbi, true, &box) == 1);
		assert(box.y1 == 0.0F && box.y2 == 0.875F); // collision: floor-seated, 7/8 tall
		assert(box.x1 == 0.0F && box.x2 == 1.0F && box.z1 == 0.0F
			   && box.z2 == 1.0F);												// full x/z
		assert(block_soulsand.getBoundingBox(&bbi, false, &box) == 1 && box.y2 == 1.0F); // selection full
		assert(block_sand.getBoundingBox(&bbi, true, &box) == 1 && box.y2 == 1.0F);		 // sand full cube
		printf("  soul sand: collision box 7/8 tall (0.875), selection box full cube OK\n");
	}

	// ===================== SOUL SAND slowdown (Beta Entity.moveEntity dispatch) ===================
	{
		// Beta Entity.moveEntity iterates every cell the post-move AABB intersects
		// (floor(min+0.001)..floor(max-0.001)) and dispatches onEntityCollidedWithBlock PER
		// CELL; BlockSoulSand does motionX/Z *= 0.4 each time. So standing centered on one
		// soul-sand block multiplies once; a box straddling a seam between two multiplies
		// twice (0.4^2) -- authentic Beta. Standing on a FULL-height block puts the feet at
		// an integer y, so floor(feet+0.001) is the cell ABOVE it: no dispatch.
		// [decompile 740c583 Entity.moveEntity + BlockSoulSand.onEntityCollidedWithBlock]
		const int sx = 16, sz = 28;
		set(sx, Y, sz, BLOCK_SOULSAND, 0);
		set(sx + 1, Y, sz, BLOCK_SOULSAND, 0);
		set(20, Y, sz, BLOCK_STONE, 0);

		struct entity ent;
		memset(&ent, 0, sizeof(ent));
		ent.on_server = true;
		ent.world = &s.worlds[0];

		const float feet = (float)Y + 0.875F; // standing ON soul sand (the 7/8 box top)
		float vx, vz, expect;

		// A: centered on ONE soul-sand block -> exactly one *0.4.
		struct AABB box = {.x1 = sx + 0.2F, .y1 = feet, .z1 = sz + 0.2F,
						   .x2 = sx + 0.8F, .y2 = feet + 1.8F, .z2 = sz + 0.8F};
		vx = 1.0F, vz = -0.5F;
		entity_soulsand_slowdown(&ent, &box, &vx, &vz);
		assert(vx == 1.0F * 0.4F && vz == -0.5F * 0.4F);

		// B: straddling the seam between TWO soul-sand blocks -> *0.4 per cell (0.4^2).
		box.x1 = sx + 0.7F;
		box.x2 = sx + 1.3F;
		vx = 1.0F, vz = 1.0F;
		entity_soulsand_slowdown(&ent, &box, &vx, &vz);
		expect = 1.0F;
		expect *= 0.4F;
		expect *= 0.4F;
		assert(vx == expect && vz == expect);

		// C: standing on FULL-height stone (feet at integer y) -> the scanned cell is the
		// air above the stone, not the stone: velocity unchanged.
		box.x1 = 20.2F, box.x2 = 20.8F;
		box.y1 = (float)(Y + 1), box.y2 = (float)(Y + 1) + 1.8F;
		vx = 1.0F, vz = 1.0F;
		entity_soulsand_slowdown(&ent, &box, &vx, &vz);
		assert(vx == 1.0F && vz == 1.0F);
		printf("  soul sand: slowdown 0.4/cell (1 cell, 2-cell seam), none on full blocks OK\n");
	}

	// ===================== WATER BUCKET vaporizes in the NETHER (Beta ItemBucket) =================
	{
		// Beta ItemBucket.onItemRightClick: `worldProvider.isHellWorld && isFull ==
		// Block.waterMoving.blockID` -> random.fizz + smoke, NO block placed, and the bucket
		// STILL empties; the overworld branch places the water. Only water vaporizes (lava is
		// unaffected). The branch reads only world.dimension, so flip the field on the live
		// test world for the nether case and restore it. [decompile 740c583 ItemBucket]
		const int bx = 24, bz = 12;
		set(bx, Y, bz, BLOCK_STONE, 0);
		struct block_data on_bd = {.type = BLOCK_STONE};
		struct block_info on = {.block = &on_bd, .x = bx, .y = Y, .z = bz};
		// The harness memsets `s`, so give the player a REAL inventory the way
		// server_local_create does (the bucket swap writes the held hotbar slot).
		assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player,
								INVENTORY_SIZE, 0, 0, 0));
		s.player.active_inventory = &s.player.inventory;
		const size_t slot
			= INVENTORY_SLOT_HOTBAR + inventory_get_hotbar(&s.player.inventory);
		struct item_data held;

		// NETHER: vaporizes -- target cell stays AIR, bucket swaps to empty.
		s.worlds[0].dimension = WORLD_DIM_NETHER;
		held = (struct item_data) {.id = ITEM_BUCKET_WATER, .count = 1, .durability = 0};
		inventory_set_slot(&s.player.inventory, slot, held);
		assert(items[ITEM_BUCKET_WATER]->onUseOnBlock(
			&s, &s.player, &s.player.inventory.items[slot], &on, SIDE_TOP));
		held = s.player.inventory.items[slot];
		assert(gid(bx, Y + 1, bz) == BLOCK_AIR); // no water in the nether
		assert(held.id == ITEM_BUCKET);			 // but the bucket still emptied

		// OVERWORLD control: the same click places flowing water.
		s.worlds[0].dimension = WORLD_DIM_OVERWORLD;
		held = (struct item_data) {.id = ITEM_BUCKET_WATER, .count = 1, .durability = 0};
		inventory_set_slot(&s.player.inventory, slot, held);
		assert(items[ITEM_BUCKET_WATER]->onUseOnBlock(
			&s, &s.player, &s.player.inventory.items[slot], &on, SIDE_TOP));
		held = s.player.inventory.items[slot];
		assert(gid(bx, Y + 1, bz) == BLOCK_WATER_FLOW);
		assert(held.id == ITEM_BUCKET);
		set(bx, Y + 1, bz, BLOCK_AIR, 0); // clean up so later sections stay dry
		printf("  water bucket: vaporizes in the nether (bucket still empties), places in "
			   "overworld OK\n");
	}

	// ===================== GLOWSTONE requires a pickaxe to drop (Beta Material.rock) ==============
	// Beta glowStone is Material.rock, which REQUIRES a tool; ItemPickaxe.canHarvestBlock returns true
	// for Material.rock (hand/shovel/axe/sword do not), so glowstone drops dust ONLY with a pickaxe.
	// Our break-path drop gate (server_local.c) keys on .digging.tool/.min, so glowstone must carry the
	// same PICKAXE/WOOD config as the other rock blocks; TOOL_TYPE_ANY dropped it by hand (a divergence).
	// [decompile 740c583 Block.glowStone(Material.rock) + ItemPickaxe.canHarvestBlock]
	assert(block_glowstone.digging.tool == TOOL_TYPE_PICKAXE);
	assert(block_glowstone.digging.min == TOOL_TIER_WOOD);
	assert(block_stone.digging.tool == TOOL_TYPE_PICKAXE); // same rock-block convention
	printf("  glowstone: pickaxe-gated drop (Material.rock), like stone OK\n");

	// ===================== BED SLEEP STORES THE PERSONAL HEAD COORDINATE =====================
	{
		const int fx = 4, fy = 110, fz = 4;
		set(fx, fy, fz, BLOCK_BED, 0);
		set(fx, fy, fz + 1, BLOCK_BED, 8);
		struct block_data foot = {.type = BLOCK_BED, .metadata = 0};
		struct block_info foot_info = {
			.block = &foot, .x = fx, .y = fy, .z = fz,
		};
		s.worlds[0].dimension = WORLD_DIM_OVERWORLD;
		s.player.has_pos = true;
		s.player.health = MAX_PLAYER_HEALTH;
		s.player.x = fx + 0.5;
		s.player.y = fy + PLAYER_EYE_HEIGHT;
		s.player.z = fz + 0.5;
		s.player.has_bed_spawn = false;
		s.world_time = 1000; /* day: no successful sleep, no spawn update */
		uint32_t msgs0 = server_local_hud_messages_sent();
		blocks[BLOCK_BED]->onRightClick(
			&s, &s.player, NULL, NULL, &foot_info, SIDE_TOP);
		assert(!s.player.has_bed_spawn && s.world_time == 1000);
		assert(!s.player.sleeping);
		/* P4(a): the daytime refusal is no longer SILENT -- exactly one Beta string
		 * (tile.bed.noSleep "You can only sleep at night") for an in-reach click. */
		assert(server_local_hud_messages_sent() == msgs0 + 1);

		/* P4(b): clicking at night ENTERS the sleeping state; the skip happens ~100
		 * server ticks later, NEVER instantly. The pre-fix code asserted
		 * world_time == DAY_LENGTH_TICKS right here -- that assertion inverting is the
		 * fail-before proof (the maintainer's "bed instantly turns it to day"). */
		s.world_time = 13000;
		s.player.vel_x = 0.25F;
		s.player.vel_y = -0.5F;
		s.player.vel_z = 0.75F;
		uint32_t correction_before_sleep = s.player.correction_seq;
		struct sleep_rpc_capture sleep_enter = {0};
		clin_test_set_rpc_observer(capture_sleep_rpcs, &sleep_enter);
		blocks[BLOCK_BED]->onRightClick(
			&s, &s.player, NULL, NULL, &foot_info, SIDE_TOP);
		clin_test_set_rpc_observer(NULL, NULL);
		assert(s.world_time == 13000 && "the click must not skip time");
		assert(s.player.sleeping && s.player.sleep_timer == 0);
		assert(s.player.vel_x == 0.0F && s.player.vel_y == 0.0F
			   && s.player.vel_z == 0.0F);
		assert(s.player.correction_seq != correction_before_sleep);
		/* PAR-BED-META: BlockBed.blockActivated:70-72 -- after sleepInBedAt
		 * returns OK, setBedOccupied(head, true) sets 0x4 through
		 * setBlockMetadataWithNotify, so the client sees the head's new
		 * metadata AFTER the snap + sleep state. */
		assert(sleep_enter.count == 3);
		assert(sleep_enter.calls[2].type == CRPC_SET_BLOCK);
		assert(sleep_enter.calls[2].payload.set_block.block.type == BLOCK_BED);
		assert(sleep_enter.calls[2].payload.set_block.block.metadata == (8 | 4));
		assert(gmeta(fx, fy, fz + 1) == (8 | 4)
			   && "a successful sleep marks the head occupied (0x4)");
		assert(gmeta(fx, fy, fz) == 0 && "the foot's metadata is untouched");
		assert(sleep_enter.calls[0].type == CRPC_PLAYER_POS);
		assert(sleep_enter.calls[0].payload.player_pos.correction_seq
			   == s.player.correction_seq);
		assert(sleep_enter.calls[0].payload.player_pos.correction_flags
			   == (PLAYER_CORRECTION_POSITION_ONLY
				   | PLAYER_CORRECTION_ZERO_VEL_X
				   | PLAYER_CORRECTION_ZERO_VEL_Y
				   | PLAYER_CORRECTION_ZERO_VEL_Z));
		assert(sleep_enter.calls[1].type == CRPC_SLEEP_STATE);
		assert(sleep_enter.calls[1].payload.sleep_state.sleeping);
		assert(sleep_enter.calls[1].payload.sleep_state.timer == 0);
		assert(!s.player.has_bed_spawn
			   && "the spawn is set on WAKE (wakeUpPlayer setSpawn=true), not on click");

		/* Batch 2a BODY SNAP [740c583 EntityPlayer.sleepInBedAt]: the sleeper
		 * lands IN the bed -- head cell (4,110,5), dir +Z (meta 8 -> dir 0) ->
		 * (head + 0.5, head + 0.9375, head + 0.9). The eye at bedY+0.9375 IS
		 * the in-bed camera; the correction RPC must carry the snapped
		 * position so the client camera follows. */
		assert(s.player.x == 4.5 && "sleep snap x = head + 0.5");
		assert(s.player.y == 110.9375 && "sleep snap eye = bedY + 0.9375");
		/* Beta computes the offset in FLOAT ((float)z + 0.9F), so the exact
		 * expectation is 0.9f widened -- not the decimal 5.9. */
		assert(s.player.z == 5.0 + (double)0.9f
			   && "sleep snap z = head + 0.9f (+Z head)");
		assert(sleep_enter.calls[0].payload.player_pos.position[0] == 4.5f);
		assert(sleep_enter.calls[0].payload.player_pos.position[1] == 110.9375f);
		/* The RPC position is a float vec3: the double snap narrows. */
		assert(sleep_enter.calls[0].payload.player_pos.position[2]
			   == (float)(5.0 + (double)0.9f));

		/* The same authoritative RPC drives the client freeze. A timer of zero must
		 * still mean sleeping on entry, while timer 100 after wake must not. */
		clin_process(&sleep_enter.calls[1]);
		assert(gstate.players[0].sleeping && gstate.players[0].sleep_timer == 0);
		/* Client entities are allocated from raw malloc storage. Poison the local
		 * player just like the dropped-item regression: every dispatch field must
		 * be authored by the constructor, never inherited from recycled heap. */
		/* F8 (Beta blockExists guard): entity_tick now probes e->world for
		 * the column the body stands in, so a zeroed gstate.world is no
		 * longer a valid fixture -- give the case the same minimal client
		 * world + resident column tests/test_local_player_column_guard.c
		 * uses, so the gate is OPEN and the SLEEPING freeze below is what
		 * the assertions actually measure. The body is placed at
		 * (1.25, 65.0, -3.5) further down, i.e. column (floor 1, floor -4);
		 * one write inside its y=[64,79] section installs that column. */
		world_create(&gstate.world);
		world_set_block(&gstate.world, 1, 64, -4,
						(struct block_data) {.type = BLOCK_STONE}, false);
		assert(world_find_chunk(&gstate.world, 1, 64, -4)
			   && "the body column must be resident so the F8 gate releases");

		struct entity client_sleeper;
		memset(&client_sleeper, 0xBE, sizeof(client_sleeper));
		entity_local_player(123, &client_sleeper, &gstate.world);
		assert(client_sleeper.getBoundingBox != NULL);
		assert(client_sleeper.onLeftClick == NULL
			   && client_sleeper.onRightClick == NULL);

		/* ID zero is the normal local-player ID, but exclusion belongs to the
		 * entity type: a malformed/noncanonical ID must not make the camera ray
		 * invoke or select the player's own bounding box. */
		dict_entity_t ray_entities;
		dict_entity_init(ray_entities);
		struct entity** ray_slot
			= dict_entity_safe_get(ray_entities, client_sleeper.id);
		*ray_slot = &client_sleeper;
		float local_hit_t = -1.0F;
		assert(raycast_entity(
				   &ray_entities, (vec3) {0.0F, 0.0F, 0.0F},
				   (vec3) {0.0F, 0.0F, 1.0F}, 4.5F, &local_hit_t)
			   == NULL);
		dict_entity_erase(ray_entities, client_sleeper.id);
		/* A missing callback in any future constructor must not become an
		 * indirect NULL call in Wii builds, where assert is compiled out. */
		client_sleeper.id = 124;
		client_sleeper.type = ENTITY_ITEM;
		client_sleeper.getBoundingBox = NULL;
		ray_slot = dict_entity_safe_get(ray_entities, client_sleeper.id);
		*ray_slot = &client_sleeper;
		assert(raycast_entity(
				   &ray_entities, (vec3) {0.0F, 0.0F, 0.0F},
				   (vec3) {0.0F, 0.0F, 1.0F}, 4.5F, &local_hit_t)
			   == NULL);
		dict_entity_erase(ray_entities, client_sleeper.id);
		dict_entity_clear(ray_entities);
		client_sleeper.pos[0] = 1.25F;
		client_sleeper.pos[1] = 65.0F;
		client_sleeper.pos[2] = -3.5F;
		client_sleeper.vel[0] = 0.3F;
		client_sleeper.vel[1] = -0.4F;
		client_sleeper.vel[2] = 0.5F;
		client_sleeper.orient[0] = 0.75F;
		client_sleeper.orient[1] = 1.25F;
		assert(!client_sleeper.tick_client(&client_sleeper));
		assert(client_sleeper.pos[0] == 1.25F
			   && client_sleeper.pos[1] == 65.0F
			   && client_sleeper.pos[2] == -3.5F);
		assert(client_sleeper.pos_old[0] == client_sleeper.pos[0]
			   && client_sleeper.pos_old[1] == client_sleeper.pos[1]
			   && client_sleeper.pos_old[2] == client_sleeper.pos[2]);
		assert(client_sleeper.vel[0] == 0.0F
			   && client_sleeper.vel[1] == 0.0F
			   && client_sleeper.vel[2] == 0.0F);
		gstate.players[0].digging.active = true;
		assert(screen_ingame_test_gate_sleep_actions());
		assert(!gstate.players[0].digging.active);
		struct client_rpc client_wake = {
			.type = CRPC_SLEEP_STATE,
			.payload.sleep_state = {.timer = 100, .sleeping = false},
		};
		clin_process(&client_wake);
		assert(!gstate.players[0].sleeping && gstate.players[0].sleep_timer == 100);
		assert(!screen_ingame_test_gate_sleep_actions());
		gstate.players[0].sleep_timer = 0;

		/* sleepInBedAt starts with isPlayerSleeping -> OTHER_PROBLEM. A second
		 * activation must not reset a partially elapsed timer. */
		for(int t = 0; t < 7; t++)
			server_local_sleep_tick(&s);
		assert(s.player.sleep_timer == 7);
		blocks[BLOCK_BED]->onRightClick(
			&s, &s.player, NULL, NULL, &foot_info, SIDE_TOP);
		assert(s.player.sleeping && s.player.sleep_timer == 7);

		/* The authoritative isMovementBlocked equivalent: neither a position
		 * sample nor a world action may get through while sleeping. */
		s.player.finished_loading = true;
		s.player.correction_seq = 77;
		double sleeping_x = s.player.x;
		struct server_rpc move = {
			.type = SRPC_PLAYER_POS,
			.payload.player_pos = {
				.x = sleeping_x + 1.0,
				.y = s.player.y,
				.z = s.player.z,
				.rx = 0.0F,
				.ry = 0.0F,
				.vel_y = 0.0F,
				.on_ground = true,
				.sneaking = false,
				.correction_seq = 77,
			},
		};
		server_local_test_process_rpc(&s, &move);
		assert(s.player.x == sleeping_x);
		set(fx + 5, fy, fz, BLOCK_STONE, 0);
		struct server_rpc dig = {
			.type = SRPC_BLOCK_DIG,
			.payload.block_dig = {
				.finished = true,
				.x = fx + 5,
				.y = fy,
				.z = fz,
				.side = SIDE_TOP,
			},
		};
		server_local_test_process_rpc(&s, &dig);
		assert(gid(fx + 5, fy, fz) == BLOCK_STONE);

		/* EntityPlayer.attackEntityFrom wakes BEFORE EntityLiving's i-frame
		 * arbitration. Even an absorbed hit therefore ends sleep immediately. */
		short health_before_wake = s.player.health;
		s.player.hurt_resist_time = 5;
		s.player.last_damage = 40;
		assert(server_local_damage_player(&s, 16, 0.0F, 0.0F));
		assert(!s.player.sleeping && s.player.sleep_timer == 0);
		assert(s.player.health == health_before_wake);
		assert(!s.player.has_bed_spawn);
		/* Batch 2a WAKE REPOSITION [740c583 EntityPlayer.wakeUpPlayer]: the
		 * bed floats in open air here (no standable side cell: below is air),
		 * so Beta's fallback places the waker ATOP the head block --
		 * (head+0.5, headY+1 + yOffset+0.1 as the standing eye, head+0.5). */
		assert(s.player.x == 4.5 && s.player.z == 5.5
			   && "wake fallback centers atop the bed head");
		assert(fabs(s.player.y - (110.0 + 1.0 + 0.1 + PLAYER_EYE_HEIGHT))
				   < 1e-9
			   && "wake eye = bedY+1 + yOffset+0.1 + eye height");
		/* EntityPlayer.wakeUpPlayer:618-619: setBedOccupied(bed, false). */
		assert(gmeta(fx, fy, fz + 1) == 8 && "wake clears the occupied bit");
		s.player.hurt_resist_time = 0;
		s.player.last_damage = 0;

		/* A bed destroyed on the VERY FIRST sleep tick changes sleeping true->false
		 * while timer remains 0. The state bit, not merely a timer delta, must make
		 * that wake reach the client. */
		blocks[BLOCK_BED]->onRightClick(
			&s, &s.player, NULL, NULL, &foot_info, SIDE_TOP);
		assert(s.player.sleeping && s.player.sleep_timer == 0);
		set(fx, fy, fz + 1, BLOCK_AIR, 0);
		struct sleep_rpc_capture first_tick_wake = {0};
		clin_test_set_rpc_observer(capture_sleep_rpcs, &first_tick_wake);
		server_local_sleep_tick(&s);
		clin_test_set_rpc_observer(NULL, NULL);
		assert(!s.player.sleeping && s.player.sleep_timer == 0);
		assert(first_tick_wake.count == 1);
		assert(first_tick_wake.calls[0].type == CRPC_SLEEP_STATE);
		assert(!first_tick_wake.calls[0].payload.sleep_state.sleeping);
		assert(first_tick_wake.calls[0].payload.sleep_state.timer == 0);
		set(fx, fy, fz + 1, BLOCK_BED, 8);

		/* Re-enter for the normal fully-asleep path exercised below. */
		blocks[BLOCK_BED]->onRightClick(
			&s, &s.player, NULL, NULL, &foot_info, SIDE_TOP);
		assert(s.player.sleeping && s.player.sleep_timer == 0);
		for(int t = 0; t < 99; t++) {
			server_local_sleep_tick(&s);
			assert(s.world_time == 13000 && "no skip before fully asleep (timer 100)");
		}
		server_local_sleep_tick(&s); /* >= 100: skip + wake + spawn */
		/* next-gaps-0924/w1: Beta's skip lands on the next dawn D and World.tick:1765-1770
		 * advances the clock once more in the same tick -> D + 1 (World.tick:1748-1751). */
		assert(s.world_time == DAY_LENGTH_TICKS + 1);
		assert(!s.player.sleeping);
		assert(s.player.has_bed_spawn);
		assert(s.player.bed_spawn_x == fx && s.player.bed_spawn_y == fy
			   && s.player.bed_spawn_z == fz + 1);

		/* The WAKE FADE-OUT tail [740c583 EntityPlayer.wakeUpPlayer:636-640 +
		 * onUpdate:73-78]. wakeUpAllPlayers passes immediately=FALSE, which PINS
		 * sleepTimer at 100 so the else-branch runs it 100->110 and only then to 0;
		 * that tail is what fades the screen back in. The pre-fix code zeroed the
		 * timer on every wake, so the client had nothing to fade with and a bed
		 * click was invisible for its entire wind-up ("bed mechanics are delayed",
		 * 2026-07-30 console run) -- this assert inverting is the fail-before. */
		assert(s.player.sleep_timer == 100
			   && "a non-immediate wake pins the timer at 100 for the fade-out");
		for(int t = 0; t < 9; t++) {
			server_local_sleep_tick(&s);
			assert(s.player.sleep_timer > 100 && s.player.sleep_timer < 110
				   && "the tail climbs 101..109 while the fade plays out");
			assert(!s.player.sleeping && "the tail must not re-enter sleep");
		}
		server_local_sleep_tick(&s); /* reaches 110 -> 0 */
		assert(s.player.sleep_timer == 0 && "the tail ends at exactly 110 ticks");
		assert(s.world_time == DAY_LENGTH_TICKS + 1 && "the tail must not skip time");

		/* Bed destroyed mid-sleep: wake, NO skip, spawn UNTOUCHED
		 * (Beta wakeUpPlayer(true,true,false), EntityPlayer.java:67-68). */
		s.player.has_bed_spawn = false;
		s.world_time = 13000 + DAY_LENGTH_TICKS;
		blocks[BLOCK_BED]->onRightClick(
			&s, &s.player, NULL, NULL, &foot_info, SIDE_TOP);
		assert(s.player.sleeping);
		for(int t = 0; t < 50; t++)
			server_local_sleep_tick(&s);
		set(fx, fy, fz + 1, BLOCK_AIR, 0); /* delete the HEAD being slept in */
		server_local_sleep_tick(&s);
		assert(!s.player.sleeping);
		assert(s.world_time == 13000 + DAY_LENGTH_TICKS && "no skip on a destroyed bed");
		assert(!s.player.has_bed_spawn && "destroyed-bed wake must NOT set the spawn");
		/* restore the bed for the head-click case below */
		set(fx, fy, fz + 1, BLOCK_BED, 8);
		s.world_time = 13000;

		/* Clicking the head stores exactly the same coordinate. The head
		 * carries a STALE occupied bit (no one sleeps in it): Beta's
		 * blockActivated:48-68 finds no sleeper there, clears the bit, and
		 * sleeps normally; the wake clears it again. */
		set(fx, fy, fz + 1, BLOCK_BED, 8 | 4);
		struct block_data head = {.type = BLOCK_BED, .metadata = 8 | 4};
		struct block_info head_info = {
			.block = &head, .x = fx, .y = fy, .z = fz + 1,
		};
		s.player.bed_spawn_x = 999;
		s.world_time = 37000;
		blocks[BLOCK_BED]->onRightClick(
			&s, &s.player, NULL, NULL, &head_info, SIDE_TOP);
		for(int t = 0; t <= 100; t++)
			server_local_sleep_tick(&s); /* sleep through to the wake */
		assert(s.player.bed_spawn_x == fx && s.player.bed_spawn_y == fy
			   && s.player.bed_spawn_z == fz + 1);
		assert(gmeta(fx, fy, fz + 1) == 8
			   && "stale occupied bit: cleared, slept, cleared again on wake");

		/* PAR-BED-META: beds use Beta's own meta&3 direction over
		 * headBlockToFootBlockMap. Guard every mapping (foot clicked, head
		 * found at foot + map[d]) so the stored spawn is Beta's head cell. */
		static const int direction[4][2] = {
			{0, 1}, {-1, 0}, {0, -1}, {1, 0},
		};
		for(int d = 1; d < 4; d++) {
			int x = fx + d * 4;
			int z = fz + 4;
			s.player.x = x + 0.5;
			s.player.y = fy + PLAYER_EYE_HEIGHT;
			s.player.z = z + 0.5;
			set(x, fy, z, BLOCK_BED, d);
			set(x + direction[d][0], fy, z + direction[d][1], BLOCK_BED,
				d | 8);
			struct block_data oriented_foot = {
				.type = BLOCK_BED, .metadata = (uint8_t)d,
			};
			struct block_info oriented_info = {
				.block = &oriented_foot, .x = x, .y = fy, .z = z,
			};
			s.world_time = 13000 + (uint64_t)d * DAY_LENGTH_TICKS;
			blocks[BLOCK_BED]->onRightClick(
				&s, &s.player, NULL, NULL, &oriented_info, SIDE_TOP);
			/* the spawn is recorded at WAKE now -- sleep through */
			for(int t = 0; t <= 100; t++)
				server_local_sleep_tick(&s);
			assert(s.player.bed_spawn_x == x + direction[d][0]
				   && s.player.bed_spawn_y == fy
				   && s.player.bed_spawn_z == z + direction[d][1]);
		}

		/* EntityPlayer.sleepInBedAt accepts exactly +/-3 X/Z and +/-2 feet Y.
		 * A farther authoritative player must neither advance time nor remotely
		 * replace the saved personal spawn. */
		s.player.x = fx + 3.01;
		s.player.y = fy + PLAYER_EYE_HEIGHT;
		s.player.z = fz + 1.0;
		s.player.bed_spawn_x = 321;
		s.world_time = 61000;
		blocks[BLOCK_BED]->onRightClick(
			&s, &s.player, NULL, NULL, &head_info, SIDE_TOP);
		assert(s.world_time == 61000 && s.player.bed_spawn_x == 321);

		/* A clicked non-head whose counterpart is gone returns before sleep. */
		set(fx + 3, fy, fz, BLOCK_BED, 0);
		s.player.bed_spawn_x = 321;
		s.world_time = 61000;
		struct block_info half_info = foot_info;
		half_info.x = fx + 3;
		blocks[BLOCK_BED]->onRightClick(
			&s, &s.player, NULL, NULL, &half_info, SIDE_TOP);
		assert(s.world_time == 61000 && s.player.bed_spawn_x == 321);
		puts("  bed: night use normalizes foot/head to optional personal spawn; day/half refuse");
		/* F8 (Beta blockExists guard): matching destroy for the client world
		 * this case now creates. */
		world_destroy(&gstate.world);
	}

	// ============== NO-BED RESPAWN ANTI-SUFFOCATION (2026-08-26 fork-audit gap) =============
	// Beta ServerConfigurationManager.playerLoggedIn/recreatePlayerEntity [740c583
	// minecraft_server/net/minecraft/src/ServerConfigurationManager.java:82-84 / :155-157]
	// share ONE push-up-while-colliding loop applied right after the world-spawn/bed XZ+Y
	// decision:
	//     while(world.getCollidingBoundingBoxes(var, var.boundingBox).size() != 0)
	//         var.setPosition(var.posX, var.posY + 1.0D, var.posZ);
	// CaveWii's no-bed respawn plan (server_local_prepare_respawn_plan) built spawn_x+0.5,
	// spawn_y+EYE_HEIGHT, spawn_z+0.5 with NO clearance check at all -- a spawn column
	// buried under terrain silently suffocated the fresh body. Fixed via the shared
	// server_local_anti_suffocate_column push-up scan. The 90-97 Y band and x=1 column here
	// are untouched by every other section in this file (those live at Y=70 or fy=110).
	{
		// Primary: bury the stored spawn column under 5 solid blocks (Y 90..94), air above.
		const int sx = 1, sz = 20;
		for(int by = 90; by <= 94; by++)
			set(sx, by, sz, BLOCK_STONE, 0);
		set(sx, 95, sz, BLOCK_AIR, 0);
		set(sx, 96, sz, BLOCK_AIR, 0);

		s.worlds[0].dimension = WORLD_DIM_OVERWORLD;
		s.player.has_pos = true;
		s.player.finished_loading = true;
		s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
		s.player.active_inventory = &s.player.inventory;
		// Neutralize the death item-drop loop -- an earlier section's leftover
		// held/hotbar item is otherwise dropped as an incidental entity that has
		// nothing to do with this respawn-placement assertion.
		for(size_t slot = 0; slot < INVENTORY_SIZE; slot++)
			inventory_clear_slot(&s.player.inventory, slot);
		inventory_clear_picked_item(&s.player.inventory);
		s.player.x = 500.5;
		s.player.y = 200.0; // wherever they died -- irrelevant to the spawn placement
		s.player.z = 500.5;
		s.player.spawn_x = sx;
		s.player.spawn_y = 90;
		s.player.spawn_z = sz;
		s.player.has_bed_spawn = false;

		assert(server_local_set_player_health(&s, 0)
			   && "the shared lethal-damage funnel must accept the kill");
		assert(server_local_respawn_request(&s)
			   && "the explicit respawn must run the transaction");
		// int + float = float, the SAME precision server_local_respawn_anti_suffocate
		// uses (w_coord_t clear_y + PLAYER_EYE_HEIGHT) -- an exact `==`, not an epsilon,
		// is the correct check: a fabs/epsilon comparison here would have hidden the
		// double-rounding regression the fix's first draft actually shipped (caught by
		// test_portal_travel.c's own exact-equality malformed-spawn assert instead).
		assert(s.player.y == 95 + PLAYER_EYE_HEIGHT
			   && "a buried no-bed spawn column must push the fresh body up to the "
				  "first AIR/AIR gap above the obstruction (Y 95), not spawn "
				  "embedded in terrain (pre-fix: spawn_y(90) + EYE_HEIGHT)");
		assert(s.player.x == sx + 0.5 && s.player.z == sz + 0.5
			   && "the push-up must not drift the XZ column");
		puts("  respawn: no-bed anti-suffocation pushes the fresh body above buried "
			 "spawn terrain (740c583 ServerConfigurationManager push-up loop)");

		// Control: an ALREADY-CLEAR spawn column must be unchanged -- no drift on the
		// ordinary, unobstructed path. A completed death->respawn leaves
		// world_switch_phase non-IDLE and finished_loading false (this headless
		// harness drives no further ticks to clear them, same as the pig_saddle/
		// boat env precedent) -- reset explicitly before the second transaction.
		const int cx3 = 1, cz3 = 22;
		set(cx3, 90, cz3, BLOCK_AIR, 0);
		set(cx3, 91, cz3, BLOCK_AIR, 0);
		s.player.has_pos = true;
		s.player.finished_loading = true;
		s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
		s.player.x = 500.5;
		s.player.y = 200.0;
		s.player.z = 500.5;
		s.player.spawn_x = cx3;
		s.player.spawn_y = 90;
		s.player.spawn_z = cz3;
		s.player.has_bed_spawn = false;
		assert(server_local_set_player_health(&s, 0));
		assert(server_local_respawn_request(&s));
		assert(s.player.y == 90 + PLAYER_EYE_HEIGHT
			   && "an already-clear spawn column must NOT drift from the un-pushed Y "
				  "(no regression for the ordinary respawn path)");
		assert(s.player.x == cx3 + 0.5 && s.player.z == cz3 + 0.5);
		puts("  respawn: an already-clear spawn column is unchanged (no push-up drift)");

		s.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
		s.player.has_pos = true;
		s.player.finished_loading = true;
	}

	// ===================== BED EXPLODES IN THE NETHER (Beta BlockBed) =====================
	// LAST world-mutating section: the power-5 blasts crater the test terrain.
	{
		// Beta BlockBed.onBlockActivated where canRespawnHere()==false (the nether):
		// no sleep, no message -- the HEAD is removed explicitly and a power-5.0
		// FLAMING explosion detonates one cell past the head (the foot is left for
		// the blast). Flaming explosions set fire on ~1/3 of destroyed air-over-
		// opaque cells with a SEPARATE RNG (Beta ExplosionRNG); non-flaming ones
		// never place fire. [decompile 740c583 BlockBed.onBlockActivated:30-45 +
		// WorldProviderHell.canRespawnHere:45 + Explosion.doExplosionB:110-122]
		// The superflat harness ground is at y 0..3, so Y-level blasts float in
		// air with nothing to crater -- and the flaming fire pass only qualifies
		// destroyed AIR cells sitting on an OPAQUE cube (Beta's crater floor).
		// Give each blast a real two-layer stone field, like the netherrack the
		// nether bed actually detonates in.
		const int fx = 18, fz = 28; // bed foot; head at fz+1 (meta 0 = +Z)
		for(int px = 12; px <= 24; px++)
			for(int pz = 23; pz <= 31; pz++) { // loaded chunks span [0,32)
				set(px, Y - 2, pz, BLOCK_STONE, 0);
				set(px, Y - 1, pz, BLOCK_STONE, 0);
			}
		set(fx, Y, fz, BLOCK_BED, 0);		  // foot, direction +Z
		set(fx, Y, fz + 1, BLOCK_BED, 0 | 8); // head
		struct block_data foot_bd = {.type = BLOCK_BED, .metadata = 0};
		struct block_info foot_bi = {.block = &foot_bd, .x = fx, .y = Y, .z = fz};

		s.worlds[0].dimension = WORLD_DIM_NETHER;
		s.player.x = 0.5;
		s.player.y = Y + PLAYER_EYE_HEIGHT;
		s.player.z = 0.5;
		blocks[BLOCK_BED]->onRightClick(&s, &s.player, NULL, NULL, &foot_bi, SIDE_TOP);
		assert(gid(fx, Y, fz) == BLOCK_BED
			   && gid(fx, Y, fz + 1) == BLOCK_BED);
		s.player.x = fx + 0.5;
		s.player.y = Y + PLAYER_EYE_HEIGHT;
		s.player.z = fz + 0.5;
		blocks[BLOCK_BED]->onRightClick(&s, &s.player, NULL, NULL, &foot_bi, SIDE_TOP);
		assert(gid(fx, Y, fz + 1) != BLOCK_BED); // head removed explicitly
		assert(gid(fx, Y, fz) != BLOCK_BED);	 // foot destroyed by the blast
		// The blast cratered SOMETHING (power 5 over stone terrain).
		int air_after = 0;
		for(int dx2 = -3; dx2 <= 3; dx2++)
			for(int dz2 = -3; dz2 <= 3; dz2++)
				if(gid(fx + dx2, Y - 1, fz + 2 + dz2) == BLOCK_AIR)
					air_after++;
		assert(air_after > 0);

		// Fire: count across the bed blast + one direct flaming blast, each over a
		// stone field (P(zero fire across both) < 1e-9 with dozens of qualifying
		// crater-floor cells at 1/3 each).
		for(int px = 0; px <= 9; px++)
			for(int pz = 23; pz <= 31; pz++) {
				set(px, Y - 2, pz, BLOCK_STONE, 0);
				set(px, Y - 1, pz, BLOCK_STONE, 0);
			}
		server_world_begin_update(&s.worlds[0]);
		server_world_explode_flaming(
			&s.worlds[0], (vec3) {4.5f, (float)Y + 0.5f, 28.5f}, 4.0f);
		int fire = 0;
		for(int bx2 = 0; bx2 <= 24; bx2++)
			for(int by2 = Y - 2; by2 <= Y + 4; by2++)
				for(int bz2 = 24; bz2 <= 36; bz2++)
					if(gid(bx2, by2, bz2) == BLOCK_FIRE)
						fire++;
		/* next-gaps-0924/G2b: the decompile's ORDER -- isFlaming lights the
		 * air-over-opaque cells of the destroyed set at the END of
		 * doExplosionA, then doExplosionB removes every set cell with id > 0,
		 * the fresh fire included -- so no fire from the blast's own set
		 * survives. (mc173 keeps a pre-fire air flag and preserves its fire;
		 * decompile > mc173. Oracle card: verify against the owned .minecraft
		 * b1.7.3.) [740c583 Explosion.doExplosionA:107-122, doExplosionB:157-160] */
		assert(fire == 0);

		// Control: a NON-flaming explosion never places fire (deterministic), even
		// when it craters the same kind of stone field the flaming ones did.
		const int cx2 = 26, cz2 = 4; // field must stay inside [0,32)
		for(int dx2 = -5; dx2 <= 5; dx2++) // fresh stone field far from the fires
			for(int dz2 = -4; dz2 <= 4; dz2++) {
				set(cx2 + dx2, Y - 2, cz2 + dz2, BLOCK_STONE, 0);
				set(cx2 + dx2, Y - 1, cz2 + dz2, BLOCK_STONE, 0);
			}
		server_world_begin_update(&s.worlds[0]);
		server_world_explode(&s.worlds[0], (vec3) {(float)cx2 + 0.5f, (float)Y + 0.5f,
										 (float)cz2 + 0.5f},
							 3.0f);
		for(int bx2 = cx2 - 7; bx2 <= cx2 + 7; bx2++)
			for(int by2 = Y - 2; by2 <= Y + 3; by2++)
				for(int bz2 = 0; bz2 <= cz2 + 7; bz2++)
					assert(gid(bx2, by2, bz2) != BLOCK_FIRE);
		s.worlds[0].dimension = WORLD_DIM_OVERWORLD;
		printf("  bed: explodes flaming in the nether (head removed, crater, fire); "
			   "non-flaming explosions never place fire OK\n");
	}

	// ============ GRASS updateTick RNG draw order + range [740c583 BlockGrass.updateTick] ============
	// Decompile 740c583 (fetched fresh for this fix, see block_grass.c's own citation)
	// shows Beta DOES roll `rand.nextInt(4) != 0 -> return` before the dirt conversion
	// (contra a "no probability roll" claim in the fix-lane brief), and the spread is a
	// SINGLE per-tick attempt (not a loop) with y-offset `nextInt(5) - 3`, i.e. -3..1 --
	// NOT the old `rand_gen_range(-1,5)` bug's -1..4.
	{
		const int GX = 10, GY = 120, GZ = 10;

		// ---- Dirt conversion is GATED by a 1-in-4 roll, not unconditional ----
		// Fixture fact (this engine's lighting -- lighting.c's flood): a lone opaque
		// block sitting directly on the grass (nothing propagates INTO an opaque
		// cell, and it is not >= the column's open-sky height since something IS
		// on top of it) reads sky_light==torch_light==0, so light<4 and
		// opacity(stone)=15>2 both hold with no extra sealing needed.
		set(GX, GY, GZ, BLOCK_GRASS, 0);
		set(GX, GY + 1, GZ, BLOCK_STONE, 0);
		struct block_data roof;
		assert(server_world_get_block(&s.worlds[0], GX, GY + 1, GZ, &roof));
		assert(roof.sky_light == 0 && roof.torch_light == 0
			   && "fixture: a lone opaque roof must read dark in this engine's "
				  "lighting model");

		int hit = 0, miss = 0;
		for(int trial = 0; trial < 40 && (hit == 0 || miss == 0); trial++) {
			set(GX, GY, GZ, BLOCK_GRASS, 0); // reset every trial
			struct random_gen predict = s.rand_src;
			int roll = rand_gen_int(&predict, 4);
			struct block_data gd = {.type = BLOCK_GRASS, .metadata = 0};
			struct block_info ginfo = {.block = &gd, .x = GX, .y = GY, .z = GZ};
			block_grass.onRandomTick(&s, &ginfo);
			if(roll == 0) {
				assert(gid(GX, GY, GZ) == BLOCK_DIRT
					   && "roll==0 (the 1-in-4 hit) must convert to dirt");
				hit++;
			} else {
				assert(gid(GX, GY, GZ) == BLOCK_GRASS
					   && "a missed roll must leave the grass untouched -- Beta "
						  "really does gate this conversion on a random draw");
				miss++;
			}
		}
		assert(hit > 0 && miss > 0
			   && "both roll outcomes must be reachable within 40 trials -- proves "
				  "the conversion is genuinely probabilistic, not unconditional");
		printf("  grass: dirt conversion gated by rand.nextInt(4)==0 (%d hit / %d "
			   "miss of %d trials) OK\n",
			   hit, miss, hit + miss);

		// ---- Spread: single attempt, x/z offset in -1..1, y offset in -3..1 ----
		// A) SEQUENCE/RANGE proof: exercise the exact three-draw sequence
		// (nextInt(3)-1, nextInt(5)-3, nextInt(3)-1) over many live draws, with no
		// world interaction -- must stay in range on every draw, and must actually
		// REACH y<=-2, a value the old rand_gen_range(-1,5) bug (giving -1..4)
		// could never produce.
		bool saw_y_le_neg2 = false;
		for(int trial = 0; trial < 60; trial++) {
			int xo = rand_gen_int(&s.rand_src, 3) - 1;
			int yo = rand_gen_int(&s.rand_src, 5) - 3;
			int zo = rand_gen_int(&s.rand_src, 3) - 1;
			assert(xo >= -1 && xo <= 1);
			assert(yo >= -3 && yo <= 1
				   && "Beta's y-offset is nextInt(5)-3, range -3..1");
			assert(zo >= -1 && zo <= 1);
			if(yo <= -2)
				saw_y_le_neg2 = true;
		}
		assert(saw_y_le_neg2 && "60 draws must reach y<=-2 at least once");
		printf("  grass: spread offset sequence x,z in -1..1, y in -3..1 (60 draws, "
			   "y<=-2 reached) OK\n");

		// B) END-TO-END proof: predict the NEXT draw on a copy of the live stream,
		// place a valid spread target at EXACTLY that predicted cell, restore the
		// stream, and run the real onRandomTick -- the spread must land there.
		set(GX, GY, GZ, BLOCK_GRASS, 0);
		set(GX, GY + 1, GZ, BLOCK_AIR, 0); // clear the dirt-conversion fixture's roof
		struct block_data open_above;
		assert(server_world_get_block(&s.worlds[0], GX, GY + 1, GZ, &open_above));
		assert(open_above.sky_light >= 9
			   && "fixture: open sky above the grass must read bright");

		struct random_gen before = s.rand_src;
		struct random_gen predict = before;
		int sxo = rand_gen_int(&predict, 3) - 1;
		int syo = rand_gen_int(&predict, 5) - 3;
		int szo = rand_gen_int(&predict, 3) - 1;
		int tx = GX + sxo, ty = GY + syo, tz = GZ + szo;
		set(tx, ty, tz, BLOCK_DIRT, 0);
		set(tx, ty + 1, tz, BLOCK_AIR, 0);
		struct block_data target_above;
		assert(server_world_get_block(&s.worlds[0], tx, ty + 1, tz, &target_above));
		assert(target_above.sky_light >= 4
			   && "fixture: the predicted target's own above cell must be bright "
				  "enough");

		s.rand_src = before;
		struct block_data gd2 = {.type = BLOCK_GRASS, .metadata = 0};
		struct block_info ginfo2 = {.block = &gd2, .x = GX, .y = GY, .z = GZ};
		block_grass.onRandomTick(&s, &ginfo2);
		assert(gid(tx, ty, tz) == BLOCK_GRASS
			   && "spread must land on the exact predicted cell");
		printf("  grass: spread lands on the exact drawn cell (offset %d,%d,%d) OK\n",
			   sxo, syo, szo);
	}

	// === MUSHROOM canBlockStay/onNeighbourBlockChange/updateTick [740c583 BlockMushroom] ===
	{
		const int MX = 10, MY = 118, MZ = 10;

		// canThisPlantGrowOnThisBlockID == opaqueCubeLookup[id]: ANY opaque cube,
		// not just grass/dirt (the old code's hardcoded restriction). Cobblestone
		// is the textbook "grows in a dark room on stone" vanilla case. Unlike the
		// grass/tallgrass fixtures above (which only need a lone roof BLOCK to read
		// dark), THIS is the mushroom's OWN cell, open on 4 sides -- side-adjacent
		// open-sky columns would otherwise leak light in horizontally, so seal it
		// on all sides too (test_sapling_canblockstay.c's proven sealed-box recipe).
		set(MX, MY - 1, MZ, BLOCK_COBBLESTONE, 0);
		set(MX - 1, MY, MZ, BLOCK_STONE, 0);
		set(MX + 1, MY, MZ, BLOCK_STONE, 0);
		set(MX, MY, MZ - 1, BLOCK_STONE, 0);
		set(MX, MY, MZ + 1, BLOCK_STONE, 0);
		set(MX, MY + 1, MZ, BLOCK_STONE, 0); // roof
		set(MX, MY, MZ, BLOCK_BROWM_MUSHROOM, 0);
		struct block_data mroof;
		assert(server_world_get_block(&s.worlds[0], MX, MY, MZ, &mroof));
		int mlight = mroof.sky_light > mroof.torch_light ? mroof.sky_light
														  : mroof.torch_light;
		assert(mlight < 13 && "fixture: the mushroom cell itself must be dark enough");
		assert(gid(MX, MY, MZ) == BLOCK_BROWM_MUSHROOM);

		// onNeighbourBlockChange: pull the cobblestone support -> pop + drop 1.
		size_t drops_before = dict_entity_size(s.entities);
		set(MX, MY - 1, MZ, BLOCK_AIR, 0);
		assert(gid(MX, MY, MZ) == BLOCK_AIR
			   && "losing its opaque-cube support must pop the mushroom");
		assert(dict_entity_size(s.entities) == drops_before + 1
			   && "the pop must drop exactly one mushroom item");
		printf("  mushroom: canBlockStay = light<13 && ANY opaque-cube support "
			   "(cobblestone, not just grass/dirt); support loss pops + drops 1 OK\n");

		// onItemPlace: any opaque cube is now a valid surface (cobblestone
		// accepted); a see-through, non-opaque surface (glass) is still rejected.
		set(MX + 3, MY - 1, MZ, BLOCK_COBBLESTONE, 0);
		struct item_data mush_it = {.id = BLOCK_BROWM_MUSHROOM, .count = 1};
		struct block_info mush_where = {.x = MX + 3, .y = MY, .z = MZ};
		assert(block_brown_mushroom.block_item.onItemPlace(
				   &s, &s.player, &mush_it, &mush_where, &mush_where, SIDE_TOP)
			   && "mushrooms must be plantable on ANY opaque cube, not just "
				  "grass/dirt");
		assert(gid(MX + 3, MY, MZ) == BLOCK_BROWM_MUSHROOM);

		set(MX + 6, MY - 1, MZ, BLOCK_GLASS, 0);
		struct item_data mush_it2 = {.id = BLOCK_BROWM_MUSHROOM, .count = 1};
		struct block_info mush_where2 = {.x = MX + 6, .y = MY, .z = MZ};
		assert(!block_brown_mushroom.block_item.onItemPlace(
				   &s, &s.player, &mush_it2, &mush_where2, &mush_where2, SIDE_TOP)
			   && "glass (see-through, not an opaque cube) must reject placement");
		printf("  mushroom: onItemPlace shares canBlockStay's opaque-cube rule "
			   "(cobblestone OK, glass rejected) OK\n");

		// updateTick: nextInt(100)==0 gates the spread; the random tick NEVER
		// self-checks canBlockStay (support loss only ever pops via
		// onNeighbourBlockChange -- see block_brown_mushroom.c's own citation).
		set(MX + 9, MY, MZ, BLOCK_BROWM_MUSHROOM, 0);
		set(MX + 9, MY - 1, MZ, BLOCK_AIR, 0); // zero support: would fail canBlockStay
		struct block_data noselfpop = {.type = BLOCK_BROWM_MUSHROOM, .metadata = 0};
		struct block_info noselfinfo
			= {.block = &noselfpop, .x = MX + 9, .y = MY, .z = MZ};
		for(int t = 0; t < 20; t++)
			block_brown_mushroom.onRandomTick(&s, &noselfinfo);
		assert(gid(MX + 9, MY, MZ) == BLOCK_BROWM_MUSHROOM
			   && "a random tick must NEVER self-pop a mushroom, even with zero "
				  "support -- only onNeighbourBlockChange does");
		printf("  mushroom: updateTick never self-checks canBlockStay OK\n");
	}

	// === TALLGRASS/DEADBUSH canBlockStay+onNeighbourBlockChange [740c583 BlockFlower/
	// BlockTallGrass/BlockDeadBush] ===
	{
		const int TX = 10, TY = 116, TZ = 10;

		// Tallgrass: stays on grass/dirt/farmland with light>=8; support loss pops.
		set(TX, TY - 1, TZ, BLOCK_GRASS, 0);
		set(TX, TY, TZ, BLOCK_TALL_GRASS, 1);
		assert(gid(TX, TY, TZ) == BLOCK_TALL_GRASS);
		server_world_light_drain(&s.worlds[0]); /* G9: the settled fixture light */
		struct block_data tg_light;
		assert(server_world_get_block(&s.worlds[0], TX, TY, TZ, &tg_light));
		int tglight = tg_light.sky_light > tg_light.torch_light ? tg_light.sky_light
																 : tg_light.torch_light;
		assert(tglight >= 8 && "fixture: open sky must give the tallgrass enough light");
		set(TX, TY - 1, TZ, BLOCK_STONE, 0); // stone is not grass/dirt/farmland
		assert(gid(TX, TY, TZ) == BLOCK_AIR
			   && "tallgrass must pop when its grass/dirt/farmland support is gone");

		// Farmland is ALSO valid soil (BlockFlower.canThisPlantGrowOnThisBlockID).
		set(TX + 2, TY - 1, TZ, BLOCK_FARMLAND, 0);
		set(TX + 2, TY, TZ, BLOCK_TALL_GRASS, 1);
		assert(gid(TX + 2, TY, TZ) == BLOCK_TALL_GRASS
			   && "farmland is valid tallgrass soil (BlockFlower default)");

		// Darkness alone (no neighbour change) pops it via the random tick. This
		// cell is open on 4 sides, so (like the mushroom fixture above) it needs
		// full side walls, not just a roof, or horizontal light from the open
		// neighbouring columns leaks in.
		set(TX + 4, TY - 1, TZ, BLOCK_GRASS, 0);
		set(TX + 3, TY, TZ, BLOCK_STONE, 0);
		set(TX + 5, TY, TZ, BLOCK_STONE, 0);
		set(TX + 4, TY, TZ - 1, BLOCK_STONE, 0);
		set(TX + 4, TY, TZ + 1, BLOCK_STONE, 0);
		set(TX + 4, TY, TZ, BLOCK_TALL_GRASS, 1);
		set(TX + 4, TY + 1, TZ, BLOCK_STONE, 0); // opaque roof -> reads dark
		struct block_data dark_tg;
		assert(server_world_get_block(&s.worlds[0], TX + 4, TY, TZ, &dark_tg));
		int darklight = dark_tg.sky_light > dark_tg.torch_light ? dark_tg.sky_light
																 : dark_tg.torch_light;
		assert(darklight < 8 && "fixture: sealed cell must read dark");
		struct block_info dark_info = {.block = &dark_tg, .x = TX + 4, .y = TY, .z = TZ};
		block_tallgrass.onRandomTick(&s, &dark_info);
		assert(gid(TX + 4, TY, TZ) == BLOCK_AIR
			   && "darkness alone (random tick) must pop tallgrass, no neighbour "
				  "change needed");
		printf("  tallgrass: grass/dirt/farmland+light>=8 canBlockStay; support-loss "
			   "and darkness both pop OK\n");

		// Dead bush: SAND only (not grass/dirt), and NEVER drops anything.
		set(TX + 6, TY - 1, TZ, BLOCK_SAND, 0);
		set(TX + 6, TY, TZ, BLOCK_DEADBUSH, 0);
		assert(gid(TX + 6, TY, TZ) == BLOCK_DEADBUSH);
		size_t db_drops_before = dict_entity_size(s.entities);
		set(TX + 6, TY - 1, TZ, BLOCK_DIRT, 0); // dirt is NOT valid dead-bush soil
		assert(gid(TX + 6, TY, TZ) == BLOCK_AIR
			   && "dead bush must pop when its sand support is gone (dirt doesn't "
				  "count)");
		assert(dict_entity_size(s.entities) == db_drops_before
			   && "BlockDeadBush.idDropped always returns -1 -- support loss must "
				  "drop NOTHING");
		printf("  dead bush: sand-only canBlockStay; support-loss pops with ZERO "
			   "drop OK\n");
	}

	// ============== CAKE canBlockStay + onNeighbourBlockChange [740c583 BlockCake] ==============
	{
		const int CX = 10, CY = 114, CZ = 10;
		set(CX, CY - 1, CZ, BLOCK_STONE, 0); // solid material -> canBlockStay true
		set(CX, CY, CZ, BLOCK_CAKE, 0);
		assert(gid(CX, CY, CZ) == BLOCK_CAKE);
		size_t cake_drops_before = dict_entity_size(s.entities);
		set(CX, CY - 1, CZ, BLOCK_AIR, 0); // non-solid -- canBlockStay fails
		assert(gid(CX, CY, CZ) == BLOCK_AIR
			   && "cake must pop to air when its solid support is pulled");
		assert(dict_entity_size(s.entities) == cake_drops_before
			   && "BlockCake.quantityDropped() == 0 -- support loss must drop "
				  "NOTHING");
		printf("  cake: canBlockStay = solid material below; support loss pops "
			   "with ZERO drop OK\n");
	}

	// ======= BED onNeighbourBlockChange [RetroMCP BlockBed.java:111-125] =======
	// PAR-BED-META: Beta's literal split. The 0x8 half (the pillow half ItemBed places at
	// foot + map[dir]) pops SILENTLY when its partner at (x,z) - map[dir] is gone; the
	// half WITHOUT 0x8 pops and drops the bed (dropBlockAsItemWithChance:184-188 passes
	// only metas without 0x8; idDropped:127-129 returns 0 for 0x8).
	{
		const int BX = 10, BY = 112, BZ = 10;
		// Direction 0 = {0,1} (+Z): foot at (BX,BY,BZ), head at (BX,BY,BZ+1).
		set(BX, BY, BZ, BLOCK_BED, 0);		// FOOT (bit 0x8 clear)
		set(BX, BY, BZ + 1, BLOCK_BED, 8); // HEAD (bit 0x8 set)
		assert(gid(BX, BY, BZ) == BLOCK_BED && gid(BX, BY, BZ + 1) == BLOCK_BED);

		// Remove the HEAD -> the FOOT pops AND drops exactly one bed item.
		size_t bed_drops_before = dict_entity_size(s.entities);
		set(BX, BY, BZ + 1, BLOCK_AIR, 0);
		assert(gid(BX, BY, BZ) == BLOCK_AIR
			   && "the foot must pop when its head disappears");
		assert(dict_entity_size(s.entities) == bed_drops_before + 1
			   && "the FOOT (no 0x8) drops EXACTLY one bed item (Beta)");

		// Fresh pair; remove the FOOT instead -> the HEAD pops SILENTLY.
		set(BX + 3, BY, BZ, BLOCK_BED, 0);
		set(BX + 3, BY, BZ + 1, BLOCK_BED, 8);
		bed_drops_before = dict_entity_size(s.entities);
		set(BX + 3, BY, BZ, BLOCK_AIR, 0);
		assert(gid(BX + 3, BY, BZ + 1) == BLOCK_AIR
			   && "the head must pop when its foot disappears");
		assert(dict_entity_size(s.entities) == bed_drops_before
			   && "the HEAD (0x8) drops NOTHING (Beta idDropped == 0)");
		printf("  bed: onNeighbourBlockChange -- head loses foot -> silent air; "
			   "foot loses head -> air + 1 bed item drop OK\n");
	}

	g_drain_run = 0;
	thread_join(&drain);
	server_world_destroy(&s.worlds[0]);
	string_clear(level);
	printf("test_block_behavior_parity: OK (reed water-adjacency, cake eating, farmland hydration, "
		   "farmland trample + crop pop, crops seed/wheat drop distribution)\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
