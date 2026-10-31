/*
	Crash-window golden for Nether death ownership.

	Drives the real close/drop/entity/archive/recovery paths through:
	  - an EntityItem allocation failure before the source checkpoint;
	  - startup recovery with its own allocation failure/retry;
	  - a partial region write, then retry;
	  - a boot-recovery region close failure before Player/journal commit;
	  - level.dat committed while journal removal fails, then restart recovery;
	  - a lethal explosion closing then destroying a live-edited container.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* A1b: fixture rows and hand-built windows name their world explicitly (the
 * slot-0 fallbacks for unstamped rows / unbound windows are gone). */
#define A1B_STAMP(s, kind, k, w) \
	((s)->te_owners.kind##_slot[k] = (w)->world_index, \
	 (s)->te_owners.kind##_gen[k] = (w)->generation)
#define A1B_BIND(player, w) ((player)->window_world = server_world_handle_of(w))
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define RMDIR(p) rmdir(p)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks.h"
#include "block/dispenser_logic.h"
#include "entity/combat.h"
#include "network/server_dispenser.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/complex_block_archive.h"
#include "network/death_drop_journal.h"
#include "network/death_drop_recovery.h"
#include "network/entity_archive.h"
#include "network/inventory_logic.h"
#include "network/region_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"
#include "platform/mem2.h"
#include "platform/thread.h"

static char s_fixture_WORLD_A[128]; /* was #define WORLD_A='test_death_drop_journal_a' (a literal) */
#define WORLD_A s_fixture_WORLD_A
static char s_fixture_WORLD_B[128]; /* was #define WORLD_B='test_death_drop_journal_b' (a literal) */
#define WORLD_B s_fixture_WORLD_B
static char s_fixture_WORLD_C[128]; /* was #define WORLD_C='test_death_drop_journal_c' (a literal) */
#define WORLD_C s_fixture_WORLD_C
static char s_fixture_WORLD_D[128]; /* was #define WORLD_D='test_death_drop_journal_d' (a literal) */
#define WORLD_D s_fixture_WORLD_D
static char s_fixture_WORLD_E[128]; /* was #define WORLD_E='test_death_drop_journal_e' (a literal) */
#define WORLD_E s_fixture_WORLD_E
static char s_fixture_WORLD_F[128]; /* was #define WORLD_F='test_death_drop_journal_f' (a literal) */
#define WORLD_F s_fixture_WORLD_F
static char s_fixture_WORLD_G[128]; /* was #define WORLD_G='test_death_drop_journal_g' (a literal) */
#define WORLD_G s_fixture_WORLD_G
static char s_fixture_WORLD_CONTAINER_AMEND[128]; /* was #define WORLD_CONTAINER_AMEND='test_death_drop_journal_h' (a literal) */
#define WORLD_CONTAINER_AMEND s_fixture_WORLD_CONTAINER_AMEND
static char s_fixture_WORLD_I[128]; /* was #define WORLD_I='test_death_drop_journal_i' (a literal) */
#define WORLD_I s_fixture_WORLD_I
static char s_fixture_WORLD_J[128]; /* was #define WORLD_J='test_death_drop_journal_j' (a literal) */
#define WORLD_J s_fixture_WORLD_J
static char s_fixture_WORLD_K[128]; /* was #define WORLD_K='test_death_drop_journal_k' (a literal) */
#define WORLD_K s_fixture_WORLD_K
static char s_fixture_WORLD_L[128]; /* was #define WORLD_L='test_death_drop_journal_l' (a literal) */
#define WORLD_L s_fixture_WORLD_L
static char s_fixture_WORLD_M[128]; /* was #define WORLD_M='test_death_drop_journal_m' (a literal) */
#define WORLD_M s_fixture_WORLD_M
static char s_fixture_WORLD_N[128]; /* was #define WORLD_N='test_death_drop_journal_n' (a literal) */
#define WORLD_N s_fixture_WORLD_N
static char s_fixture_WORLD_O[128]; /* was #define WORLD_O='test_death_drop_journal_o' (a literal) */
#define WORLD_O s_fixture_WORLD_O
static char s_fixture_WORLD_DISPENSER_LIVE[128]; /* was #define WORLD_DISPENSER_LIVE='test_death_drop_journal_p' (a literal) */
#define WORLD_DISPENSER_LIVE s_fixture_WORLD_DISPENSER_LIVE
static char s_fixture_WORLD_DISPENSER_REPLAY[128]; /* was #define WORLD_DISPENSER_REPLAY='test_death_drop_journal_q' (a literal) */
#define WORLD_DISPENSER_REPLAY s_fixture_WORLD_DISPENSER_REPLAY
static char s_fixture_WORLD_BATCH_LIVE[128];
#define WORLD_BATCH_LIVE s_fixture_WORLD_BATCH_LIVE
static char s_fixture_WORLD_BATCH_CUT[128];
#define WORLD_BATCH_CUT s_fixture_WORLD_BATCH_CUT
static volatile int drain_run = 1;
static void* drain_client(void* unused) {
	(void)unused;
	while(drain_run) {
		clin_drain_discard();
		thread_msleep(1);
	}
	clin_drain_discard();
	return NULL;
}

static void remove_world_files(const char* root) {
	char path[256];
	const char* files[] = {
		"level.dat", "level.dat_new", "level.dat_old",
		"death_drops.dat", "death_drops.dat_new", "death_drops.dat_old",
		"chests_dim-1.dat", "chests_dim-1.dat_new", "chests_dim-1.dat_old",
		"signs_dim-1.dat", "signs_dim-1.dat_new", "signs_dim-1.dat_old",
		"DIM-1/region/r.0.0.mcr", "region/r.0.0.mcr",
	};
	for(size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", root, files[i]);
		remove(path);
	}
}

static void remove_world_dirs(const char* root) {
	char path[256];
	remove_world_files(root);
	snprintf(path, sizeof(path), "%s/DIM-1/region", root);
	RMDIR(path);
	snprintf(path, sizeof(path), "%s/DIM-1", root);
	RMDIR(path);
	snprintf(path, sizeof(path), "%s/region", root);
	RMDIR(path);
	RMDIR(root);
}

static void make_world_dirs(const char* root) {
	char path[256];
	MKDIR(root);
	snprintf(path, sizeof(path), "%s/region", root);
	MKDIR(path);
	snprintf(path, sizeof(path), "%s/DIM-1", root);
	MKDIR(path);
	snprintf(path, sizeof(path), "%s/DIM-1/region", root);
	MKDIR(path);
}

static void create_level_with_hotbar(const char* root, uint16_t item_id) {
	string_t name;
	string_init_set_str(name, root);
	struct level_archive level;
	assert(level_archive_create_new(
		&level, name, 424242, (vec3) {8.5F, 70.0F, 8.5F}));
	level_archive_destroy(&level);
	assert(level_archive_create(&level, name));
	struct item_data items[INVENTORY_SIZE] = {{0}};
	items[INVENTORY_SLOT_HOTBAR]
		= (struct item_data) {.id = item_id, .count = 7};
	struct inventory inventory = {
		.items = items,
		.capacity = INVENTORY_SIZE,
	};
	int16_t health = MAX_PLAYER_HEALTH;
	assert(level_archive_write_player(
		&level, (vec3) {8.5F, 70.0F, 8.5F}, (vec2) {0}, NULL,
		WORLD_DIM_NETHER));
	assert(level_archive_write_inventory(&level, &inventory));
	assert(level_archive_write(&level, LEVEL_PLAYER_HEALTH, &health));
	assert(level_archive_write_player_fall_distance(&level, 9.5F));
	assert(level_archive_save(&level));
	level_archive_destroy(&level);
	string_clear(name);
}

static uint32_t journal_fixture_checksum(const uint8_t* bytes, size_t length) {
	uint32_t hash = UINT32_C(2166136261);
	for(size_t i = 0; i < length; i++) {
		hash ^= bytes[i];
		hash *= UINT32_C(16777619);
	}
	return hash;
}

/* Convert a just-written v3 journal into the exact v2 layout. This fixture
 * proves old crash journals remain readable and, because v2 had no final
 * Player.Spawn fields, recovery preserves the existing personal spawn. */
static void rewrite_current_journal_as_v2(const char* root) {
	char path[256];
	snprintf(path, sizeof(path), "%s/death_drops.dat", root);
	uint8_t bytes[8192];
	FILE* f = fopen(path, "rb");
	assert(f);
	size_t length = fread(bytes, 1, sizeof(bytes), f);
	assert(!ferror(f) && fgetc(f) == EOF && fclose(f) == 0);

	/* Header through final health is 39 bytes. v3 then added one spawn-state
	 * byte plus three big-endian int32 coordinates before the old record list. */
	const size_t spawn_offset = 39;
	const size_t spawn_bytes = 13;
	assert(length >= spawn_offset + spawn_bytes + 4);
	assert(bytes[4] == 0 && bytes[5] == 3);
	bytes[5] = 2;
	memmove(bytes + spawn_offset, bytes + spawn_offset + spawn_bytes,
			length - spawn_offset - spawn_bytes);
	length -= spawn_bytes;
	uint32_t checksum = journal_fixture_checksum(bytes, length - 4);
	bytes[length - 4] = (uint8_t)(checksum >> 24);
	bytes[length - 3] = (uint8_t)(checksum >> 16);
	bytes[length - 2] = (uint8_t)(checksum >> 8);
	bytes[length - 1] = (uint8_t)checksum;

	f = fopen(path, "wb");
	assert(f);
	assert(fwrite(bytes, length, 1, f) == 1 && fclose(f) == 0);
}

static void rewrite_first_v3_record_id(const char* root, uint16_t id) {
	char path[256];
	snprintf(path, sizeof(path), "%s/death_drops.dat", root);
	uint8_t bytes[8192];
	FILE* f = fopen(path, "rb");
	assert(f);
	size_t length = fread(bytes, 1, sizeof(bytes), f);
	assert(!ferror(f) && fgetc(f) == EOF && fclose(f) == 0);
	/* v3 fixed header (39) + personal-spawn payload (13), then record[0].id. */
	const size_t record_offset = 52;
	assert(length >= record_offset + 2 + 4);
	assert(bytes[4] == 0 && bytes[5] == 3);
	bytes[record_offset] = (uint8_t)(id >> 8);
	bytes[record_offset + 1] = (uint8_t)id;
	uint32_t checksum = journal_fixture_checksum(bytes, length - 4);
	bytes[length - 4] = (uint8_t)(checksum >> 24);
	bytes[length - 3] = (uint8_t)(checksum >> 16);
	bytes[length - 2] = (uint8_t)(checksum >> 8);
	bytes[length - 1] = (uint8_t)checksum;
	f = fopen(path, "wb");
	assert(f);
	assert(fwrite(bytes, length, 1, f) == 1 && fclose(f) == 0);
}

static void assert_level_fall_distance(const char* root, float expected) {
	string_t name;
	string_init_set_str(name, root);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	float fall_distance = -1.0F;
	assert(level_archive_read(
		&level, LEVEL_PLAYER_FALL_DISTANCE, &fall_distance, 0));
	assert(fall_distance == expected);
	level_archive_destroy(&level);
	string_clear(name);
}

static void init_server(struct server_local* server, const char* root) {
	memset(server, 0, sizeof(*server));
	rand_gen_seed(&server->rand_src);
	server->container_drop_rng.seed = 0x12345678u;
	string_init_set_str(server->level_name, root);
	assert(level_archive_create(&server->level, server->level_name));
	/* Mirror the production world-open counter seed (D6): absent tag ->
	 * legacy floor. Keeps harness allocations faithful to the live path. */
	if(!level_archive_read_death_tx_counter(&server->level,
											&server->death_tx_counter))
		server->death_tx_counter = CAVEWII_DEATH_TX_LEGACY_FLOOR;
	assert(inventory_create(
		&server->player.inventory, &inventory_logic_player, server,
		&server->player,
		INVENTORY_SIZE, 0, 0, 0));
	assert(level_archive_read_inventory(
		&server->level, &server->player.inventory));
	server->player.active_inventory = &server->player.inventory;
	server->player.x = 8.5;
	server->player.y = 70.0;
	server->player.z = 8.5;
	server->player.spawn_x = 8;
	server->player.spawn_y = 70;
	server->player.spawn_z = 8;
	server->player.health = MAX_PLAYER_HEALTH;
	server->player.dimension = WORLD_DIM_NETHER;
	server->player.has_pos = true;
	server->player.finished_loading = true;
	server->player.fire = -PLAYER_FIRE_RESISTANCE;
	server->player.oxygen = MAX_OXYGEN;
	server->world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(server))
		abort();
	piston_moving_store_init(&server->piston_moving);
	dict_entity_init(server->entities);
	server->world_populate_scratch = server_world_populate_scratch_create();
	assert(server->world_populate_scratch);
	assert(server_world_create_with_populate_scratch(
		&server->worlds[0], server->level_name, WORLD_DIM_NETHER, 424242, true,
		server->world_populate_scratch));
	server->worlds[0].local = server;
	server->worlds[0].session = server; /* A1a (D1): owning session for services/callbacks */
	assert(!server->death_drop_journal);
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&server->worlds[0], 0, 0, &chunk) && chunk);
}

static void free_runtime_entities(struct server_local* server) {
	dict_entity_it_t it;
	dict_entity_it(it, server->entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_clear(server->entities);
}

static void discard_server(struct server_local* server) {
	if(server->player.has_pos)
		server_world_discard(&server->worlds[0]);
	server_world_populate_scratch_destroy(server->world_populate_scratch);
	server->world_populate_scratch = NULL;
	piston_moving_store_destroy(&server->piston_moving);
	free_runtime_entities(server);
	if(server->level.data)
		level_archive_discard(&server->level);
	if(server->player.active_inventory
	   && server->player.active_inventory != &server->player.inventory)
		inventory_destroy_owned(server->player.active_inventory, &server->player);
	inventory_destroy(&server->player.inventory, &server->player);
	mem2_free_sized(
		server->entity_tick_keys,
		server->entity_tick_capacity * sizeof(*server->entity_tick_keys));
	server->entity_tick_keys = NULL;
	server->entity_tick_count = 0;
	server->entity_tick_capacity = 0;
	server->entity_tick_reserved = 0;
	server->entity_tick_active = false;
	death_drop_journal_free(server->death_drop_journal);
	server->death_drop_journal = NULL;
	string_clear(server->level_name);
}

static void free_chunk(struct server_chunk* chunk) {
	mem2_free_sized(chunk->ids, CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT);
	mem2_free_sized(chunk->metadata,
					CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2);
	mem2_free_sized(chunk->lighting_sky,
					CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2);
	mem2_free_sized(chunk->lighting_torch,
					CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2);
	mem2_free_sized(chunk->heightmap, CHUNK_SIZE * CHUNK_SIZE);
}

static size_t read_transaction_entities(const char* root, uint32_t tx,
									 bool seen[DEATH_DROP_JOURNAL_MAX_RECORDS]) {
	string_t name;
	string_init_set_str(name, root);
	struct region_archive region;
	assert(region_archive_create(&region, name, 0, 0, WORLD_DIM_NETHER));
	struct server_chunk chunk = {0};
	struct chunk_tile_entities tile_entities = {0};
	struct chunk_entities entities = {0};
	assert(region_archive_get_chunk(
		&region, 0, 0, &chunk, &tile_entities, &entities));
	size_t count = 0;
	for(size_t i = 0; i < entities.count; i++)
		if(entities.entities[i].kind == ARCHIVED_ENTITY_ITEM
		   && entities.entities[i].data.item.death_transaction == tx) {
			size_t sequence
				= entities.entities[i].data.item.death_sequence;
			assert(sequence < DEATH_DROP_JOURNAL_MAX_RECORDS);
			assert(!seen[sequence]);
			seen[sequence] = true;
			count++;
		}
	chunk_entities_free(&entities);
	tile_entities_parse_free(&tile_entities);
	free_chunk(&chunk);
	region_archive_destroy(&region);
	string_clear(name);
	return count;
}

static void assert_level_inventory_empty(const char* root) {
	string_t name;
	string_init_set_str(name, root);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	struct item_data items[INVENTORY_SIZE] = {{0}};
	struct inventory inventory = {.items = items, .capacity = INVENTORY_SIZE};
	assert(level_archive_read_inventory(&level, &inventory));
	for(size_t i = 0; i < INVENTORY_SIZE; i++)
		assert(items[i].id == 0);
	enum world_dim dimension = WORLD_DIM_NETHER;
	assert(level_archive_read_player(&level, NULL, NULL, NULL, &dimension));
	assert(dimension == WORLD_DIM_OVERWORLD);
	level_archive_destroy(&level);
	string_clear(name);
}

static struct item_data read_chest_slot(
	const char* root, w_coord_t x, w_coord_t y, w_coord_t z, size_t slot) {
	string_t name;
	string_init_set_str(name, root);
	struct region_archive region;
	assert(region_archive_create(
		&region, name, CHUNK_REGION_COORD(x / CHUNK_SIZE),
		CHUNK_REGION_COORD(z / CHUNK_SIZE), WORLD_DIM_NETHER));
	struct server_chunk chunk = {0};
	struct chunk_tile_entities tile_entities = {0};
	struct chunk_entities entities = {0};
	assert(region_archive_get_chunk(
		&region, x / CHUNK_SIZE, z / CHUNK_SIZE, &chunk,
		&tile_entities, &entities));
	bool found = false;
	struct item_data item = {0};
	for(int i = 0; i < tile_entities.n_chest; i++)
		if(tile_entities.chests[i].x == x && tile_entities.chests[i].y == y
		   && tile_entities.chests[i].z == z) {
			assert(slot < TE_CHEST_SLOTS);
			item = tile_entities.chests[i].items[slot];
			found = true;
			break;
		}
	assert(found);
	chunk_entities_free(&entities);
	tile_entities_parse_free(&tile_entities);
	free_chunk(&chunk);
	region_archive_destroy(&region);
	string_clear(name);
	return item;
}

static bool same_item(struct item_data a, struct item_data b) {
	return a.id == b.id && a.count == b.count
		&& a.durability == b.durability;
}

static void copy_file(const char* source, const char* destination) {
	FILE* in = fopen(source, "rb");
	FILE* out = fopen(destination, "wb");
	assert(in && out);
	unsigned char bytes[512];
	for(;;) {
		size_t count = fread(bytes, 1, sizeof(bytes), in);
		if(count == 0)
			break;
		assert(fwrite(bytes, 1, count, out) == count);
	}
	assert(!ferror(in));
	assert(fclose(in) == 0);
	assert(fclose(out) == 0);
}

static bool file_exists(const char* path) {
	FILE* file = fopen(path, "rb");
	if(!file)
		return false;
	fclose(file);
	return true;
}

static void assert_destroyed_chest_drop_state(
	const char* root, w_coord_t x, w_coord_t y, w_coord_t z, uint32_t tx,
	struct item_data player_stack, struct item_data live_chest_stack) {
	string_t name;
	string_init_set_str(name, root);
	struct region_archive region;
	assert(region_archive_create(
		&region, name, CHUNK_REGION_COORD(x / CHUNK_SIZE),
		CHUNK_REGION_COORD(z / CHUNK_SIZE), WORLD_DIM_NETHER));
	struct server_chunk chunk = {0};
	struct chunk_tile_entities tile_entities = {0};
	struct chunk_entities entities = {0};
	assert(region_archive_get_chunk(
		&region, x / CHUNK_SIZE, z / CHUNK_SIZE, &chunk,
		&tile_entities, &entities));

	size_t block_index = (size_t)y
		+ ((size_t)(z % CHUNK_SIZE)
		   + (size_t)(x % CHUNK_SIZE) * CHUNK_SIZE)
			* WORLD_HEIGHT;
	assert(chunk.ids[block_index] == BLOCK_AIR);
	for(int i = 0; i < tile_entities.n_chest; i++)
		assert(tile_entities.chests[i].x != x
			   || tile_entities.chests[i].y != y
			   || tile_entities.chests[i].z != z);

	size_t transaction_count = 0;
	size_t player_count = 0;
	size_t live_chest_count = 0;
	bool seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	for(size_t i = 0; i < entities.count; i++) {
		const struct archived_entity* entity = &entities.entities[i];
		if(entity->kind != ARCHIVED_ENTITY_ITEM)
			continue;
		if(same_item(entity->data.item.item, player_stack))
			player_count++;
		if(same_item(entity->data.item.item, live_chest_stack))
			live_chest_count++;
		if(entity->data.item.death_transaction != tx)
			continue;
		size_t sequence = entity->data.item.death_sequence;
		assert(sequence < DEATH_DROP_JOURNAL_MAX_RECORDS);
		assert(!seen[sequence]);
		seen[sequence] = true;
		transaction_count++;
	}
	/* Deviation-#18 ordering: the explosion destroys the chest DURING the
	 * dead dwell, so the live-edited chest stack spills through the ordinary
	 * block-destroy commit (untagged), and only the player stack rides the
	 * respawn journal. Both still exist exactly once -- the no-loss/no-dup
	 * property this golden exists for. */
	assert(transaction_count == 1);
	assert(player_count == 1);
	assert(live_chest_count == 1);

	chunk_entities_free(&entities);
	tile_entities_parse_free(&tile_entities);
	free_chunk(&chunk);
	region_archive_destroy(&region);
	string_clear(name);
}

static struct item_data read_dispenser_slot(
	const char* root, w_coord_t x, w_coord_t y, w_coord_t z, size_t slot) {
	string_t name;
	string_init_set_str(name, root);
	struct region_archive region;
	assert(region_archive_create(
		&region, name, CHUNK_REGION_COORD(x / CHUNK_SIZE),
		CHUNK_REGION_COORD(z / CHUNK_SIZE), WORLD_DIM_NETHER));
	struct server_chunk chunk = {0};
	struct chunk_tile_entities tile_entities = {0};
	struct chunk_entities entities = {0};
	assert(region_archive_get_chunk(
		&region, x / CHUNK_SIZE, z / CHUNK_SIZE, &chunk,
		&tile_entities, &entities));
	bool found = false;
	struct item_data item = {0};
	for(int i = 0; i < tile_entities.n_dispenser; i++)
		if(tile_entities.dispensers[i].x == x
		   && tile_entities.dispensers[i].y == y
		   && tile_entities.dispensers[i].z == z) {
			assert(slot < TE_DISPENSER_SLOTS);
			item = tile_entities.dispensers[i].items[slot];
			found = true;
			break;
		}
	assert(found);
	chunk_entities_free(&entities);
	tile_entities_parse_free(&tile_entities);
	free_chunk(&chunk);
	region_archive_destroy(&region);
	string_clear(name);
	return item;
}

/* Seed a live dispenser holding one stack, open its GUI, then move that stack
 * out of the 9-slot grid WITHOUT closing. The tile entity still owns the old
 * stack, so the journal's post-close snapshot is the only correct authority --
 * exactly the situation both D10 legs have to replay. */
static void open_dispenser_and_move_stack_out(
	struct server_local* server, w_coord_t x, w_coord_t y, w_coord_t z,
	struct item_data stack) {
	assert(server_world_set_block(
		&server->worlds[0], x, y, z,
		(struct block_data) {.type = BLOCK_DISPENSER, .metadata = 2}));
	assert(dispenser_te_register(server, x, y, z) >= 0);
	struct dispenser_te* te = dispenser_te_lookup(server, x, y, z);
	assert(te);
	te->items[0] = stack;

	assert(server_local_open_inventory(
		server, &inventory_logic_dispenser, DISPENSER_SIZE, x, y, z,
		WINDOWC_DISPENSER, WINDOW_TYPE_DISPENSER));
	struct inventory* window = server->player.active_inventory;
	assert(window && window != &server->player.inventory);
	struct item_data seeded;
	assert(inventory_get_slot(window, DISPENSER_SLOT_STORAGE, &seeded));
	assert(seeded.id == stack.id && seeded.count == stack.count);
	inventory_clear_slot(window, DISPENSER_SLOT_STORAGE);
	inventory_set_slot(window, DISPENSER_SLOT_HOTBAR + 1, stack);
}

/* D10: a dispenser window in a death journal used to be unrecoverable.
 *
 * D3 taught server_local_plan_death_drops to journal it and D9 taught
 * container_slot_valid to accept the records, but NOTHING could then resolve
 * WINDOWC_DISPENSER back to an owner: both the live commit
 * (server_local_apply_death_container_slots) and crash replay
 * (death_drop_recovery.c apply_container_slots) only knew the furnace and
 * chest tile entities and returned false on anything else. The live commit
 * failing meant the death checkpoint never completed and the journal was never
 * removed, so the next load handed that same journal to crash replay -- which
 * hard-failed on it too. A Nether death with a dispenser GUI open left the
 * world stuck in that loop.
 *
 * Both legs are driven here because they are separate functions with the same
 * missing arm, and only the registry's `recovery` column now ties them
 * together. */
static void test_dispenser_death_commits_and_replays(void) {
	const w_coord_t dx = 4, dy = 70, dz = 4;
	const struct item_data bread = {.id = ITEM_BREAD, .count = 4};

	/* Leg 1 -- the LIVE commit. */
	remove_world_files(WORLD_DISPENSER_LIVE);
	make_world_dirs(WORLD_DISPENSER_LIVE);
	create_level_with_hotbar(WORLD_DISPENSER_LIVE, ITEM_APPLE);
	struct server_local server;
	init_server(&server, WORLD_DISPENSER_LIVE);
	open_dispenser_and_move_stack_out(&server, dx, dy, dz, bread);

	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(server.death_drop_journal);
	/* The dispenser BLOCK survives the death, so its grid is journaled as a
	 * post-close snapshot rather than dropped as loot (D3's branch). */
	assert(server.death_drop_journal->container_count
		   == DISPENSER_SIZE_STORAGE);
	uint32_t tx = server.death_drop_journal->transaction_id;

	/* Before the fix this returned false forever: apply_death_container_slots
	 * found no arm for WINDOWC_DISPENSER, so the checkpoint never completed. */
	assert(server_local_test_death_world_reload(&server));
	assert(!server.death_drop_active && !server.death_drop_journal);
	assert(server.player.has_pos
		   && server.worlds[0].dimension == WORLD_DIM_OVERWORLD);
	discard_server(&server);

	/* The moved stack is gone from the on-disk grid and is a player drop. */
	assert(read_dispenser_slot(WORLD_DISPENSER_LIVE, dx, dy, dz, 0).id == 0);
	bool live_seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_DISPENSER_LIVE, tx, live_seen) > 0);

	/* Leg 2 -- CRASH REPLAY of the same shape. */
	remove_world_files(WORLD_DISPENSER_REPLAY);
	make_world_dirs(WORLD_DISPENSER_REPLAY);
	create_level_with_hotbar(WORLD_DISPENSER_REPLAY, ITEM_APPLE);
	init_server(&server, WORLD_DISPENSER_REPLAY);
	open_dispenser_and_move_stack_out(&server, dx, dy, dz, bread);

	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(server.death_drop_journal
		   && server.death_drop_journal->container_count
			   == DISPENSER_SIZE_STORAGE);
	tx = server.death_drop_journal->transaction_id;

	/* Crash before the checkpoint. Startup recovery owns the journal now. */
	discard_server(&server);
	string_t name;
	string_init_set_str(name, WORLD_DISPENSER_REPLAY);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	/* Before the fix this was `false`: apply_container_slots rejected the
	 * dispenser window outright, so world recovery HARD-FAILED. */
	assert(death_drop_recover_if_needed(name, &level));
	level_archive_destroy(&level);
	string_clear(name);

	assert(read_dispenser_slot(WORLD_DISPENSER_REPLAY, dx, dy, dz, 0).id == 0);
	bool seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_DISPENSER_REPLAY, tx, seen) > 0);
	assert_level_inventory_empty(WORLD_DISPENSER_REPLAY);

	puts("  death journal: dispenser window commits live and replays after a "
		 "crash (D10)");
}

static void test_crash_before_chunk_checkpoint(void) {
	remove_world_files(WORLD_A);
	make_world_dirs(WORLD_A);
	create_level_with_hotbar(WORLD_A, ITEM_APPLE);
	struct server_local server;
	init_server(&server, WORLD_A);
	inventory_set_slot(
		&server.player.inventory, INVENTORY_SLOT_CRAFTING,
		(struct item_data) {.id = BLOCK_STONE, .count = 2});
	inventory_set_picked_item(
		&server.player.inventory,
		(struct item_data) {.id = ITEM_BREAD, .count = 3});
	struct inventory* crafting = calloc(1, sizeof(*crafting));
	assert(crafting && inventory_create(
		crafting, &inventory_logic_crafting, &server, &server.player, CRAFTING_SIZE, 2, 70, 2));
	inventory_set_slot(
		crafting, CRAFTING_SLOT_INPUT,
		(struct item_data) {.id = ITEM_DYE, .durability = 4, .count = 5});
	inventory_set_picked_item(
		crafting, (struct item_data) {.id = ITEM_BREAD, .count = 1});
	server.player.active_inventory = crafting;

	server_local_test_fail_entity_alloc_after(2);
	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(server.death_drop_active && server.death_drop_prepared_durable);
	assert(server.death_drop_journal);
	assert(server.death_drop_transfer_index == server.death_drop_journal->count);
	assert(server.death_drop_journal->count == 5);
	uint32_t tx = server.death_drop_journal->transaction_id;
	string_t name;
	string_init_set_str(name, WORLD_A);
	struct death_drop_journal disk;
	assert(death_drop_journal_load(name, &disk)
		   == DEATH_DROP_JOURNAL_LOAD_OK);
	assert(disk.phase == DEATH_DROP_JOURNAL_PREPARED && disk.count == 5);
	string_clear(name);

	/* Crash now: only the pre-mutation base chunk exists. ALSO tear the
	 * allocation-time counter save (adversarial review): removing level.dat
	 * makes survivor promotion restore the PRE-COUNTER _old, so the journal's
	 * id is AHEAD of the promoted counter -- recovery's two-generation bump
	 * (commit_final_player) is the only thing that repairs it, and the
	 * counter assertions below pin exactly that. */
	discard_server(&server);
	remove(test_fixture_join(WORLD_A, "/level.dat"));
	string_init_set_str(name, WORLD_A);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	death_drop_recovery_test_fail_next_alloc();
	assert(!death_drop_recover_if_needed(name, &level));
	level_archive_discard(&level);
	assert(level_archive_create(&level, name));
	assert(death_drop_recover_if_needed(name, &level));
	level_archive_destroy(&level);
	string_clear(name);
	assert_level_inventory_empty(WORLD_A);
	assert_level_fall_distance(WORLD_A, 0.0F);
	bool seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_A, tx, seen) == 5);
	for(size_t i = 0; i < 5; i++) assert(seen[i]);

	/* D6 pins. (a) A fresh world allocates past the legacy floor (the floor
	 * clears every plausible pre-fix on-disk tag). (b) The issued id was
	 * PERSISTED to level.dat before use; recovery kept it >= the replayed
	 * journal's id. (c) A brand-new server on the same world allocates
	 * STRICTLY past the previous id even though the tx-tagged items sit in
	 * unloaded region files, invisible to the live-dict scan -- the pre-D6
	 * allocator reissued the same id in exactly this situation. */
	assert(tx > CAVEWII_DEATH_TX_LEGACY_FLOOR);
	string_init_set_str(name, WORLD_A);
	{
		struct level_archive level2;
		assert(level_archive_create(&level2, name));
		uint32_t counter = 0;
		assert(level_archive_read_death_tx_counter(&level2, &counter));
		assert(counter >= tx);
		level_archive_destroy(&level2);
	}
	string_clear(name);
	{
		struct server_local server2;
		init_server(&server2, WORLD_A);
		assert(server_local_set_player_health(&server2, 0));
		assert(server_local_respawn_request(&server2));
		assert(server2.death_drop_journal);
		assert(server2.death_drop_journal->transaction_id > tx);
		/* Production-seed pin (adversarial review): zero the mirror so ONLY
		 * the real world-open seed inside the reload's load_world_impl path
		 * can restore it -- the harness's own init_server seeding cannot mask
		 * a deleted production seed here. */
		server2.death_tx_counter = 0;
		assert(server_local_test_death_world_reload(&server2));
		assert(server2.death_tx_counter > tx);
		discard_server(&server2);
	}
	puts("  death journal: pre-chunk crash + allocation retry completes exactly once"
		 " (+ D6 persisted monotonic tx ids)");
}

static void test_partial_region_and_level_before_remove(void) {
	remove_world_files(WORLD_B);
	make_world_dirs(WORLD_B);
	create_level_with_hotbar(WORLD_B, ITEM_BREAD);
	struct server_local server;
	init_server(&server, WORLD_B);
	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(server.death_drop_journal && server.death_drop_journal->count == 1);
	uint32_t tx = server.death_drop_journal->transaction_id;

	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
	assert(!server_local_test_death_world_reload(&server));
	assert(server.death_drop_active && server.player.has_pos);
	death_drop_journal_test_fail_next(DEATH_DROP_JOURNAL_TEST_FAIL_REMOVE);
	assert(!server_local_test_death_world_reload(&server));
	/* Regions and cleared level.dat are now durable; only journal removal failed. */
	assert(server.death_drop_active && server.player.has_pos);
	discard_server(&server);

	string_t name;
	string_init_set_str(name, WORLD_B);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	assert(death_drop_recover_if_needed(name, &level));
	level_archive_destroy(&level);
	string_clear(name);
	assert_level_inventory_empty(WORLD_B);
	bool seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_B, tx, seen) == 1);
	assert(seen[0]);
	puts("  death journal: partial region + post-level/remove crash is idempotent");
}

/* F1 (audit 2026-08-04): the region fsync barrier must GATE journal removal.
   The journal is the only recovery record for the death chunks; before the
   fix, the checkpoint chain ended (chunks + level.dat + journal REMOVED) with
   every region write still in libfat's cache -- a power cut lost the chunks
   AND the record needed to rebuild them. The armed sync failure models that
   cache-flush failing: the chain must refuse, keep the journal, and retry. */
static void test_region_sync_gates_journal_removal(void) {
	remove_world_files(WORLD_B);
	make_world_dirs(WORLD_B);
	create_level_with_hotbar(WORLD_B, ITEM_BREAD);
	struct server_local server;
	init_server(&server, WORLD_B);
	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(server.death_drop_journal && server.death_drop_journal->count == 1);
	uint32_t tx = server.death_drop_journal->transaction_id;

	/* Region sync refuses -> the whole checkpoint must refuse: journal kept,
	 * death still active, nothing published. FAIL-BEFORE: without the barrier
	 * in the chain the armed failure is never consulted, the reload SUCCEEDS,
	 * and this assert fires. */
	server_local_test_fail_next_region_sync();
	assert(!server_local_test_death_world_reload(&server));
	assert(server.death_drop_active && server.player.has_pos);

	/* Sync healthy again -> the retry completes and removes the journal. */
	struct server_world_populate_scratch* session_scratch
		= server.world_populate_scratch;
	assert(server_local_test_death_world_reload(&server));
	assert(server.world_populate_scratch == session_scratch);
	assert(!server.death_drop_active);
	discard_server(&server);

	/* The completed transaction is on disk exactly once (no double-apply from
	 * the refused first attempt). */
	assert_level_inventory_empty(WORLD_B);
	bool seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_B, tx, seen) == 1);
	assert(seen[0]);
	puts("  death journal: region-sync failure blocks journal removal, retry"
		 " completes once");
}

/* Adversarial-review finding 1 on the F1 fix: BOOT-TIME RECOVERY must also
   gate journal removal on region durability. The replay writes drop chunks
   through libfat's cache, then commits a durable inventory-EMPTY level.dat and
   deletes the journal; without a sync a second power cut loses the drops AND
   the only record that could rebuild them. */
static void test_recovery_sync_gates_journal_removal(void) {
	remove_world_files(WORLD_B);
	make_world_dirs(WORLD_B);
	create_level_with_hotbar(WORLD_B, ITEM_BREAD);
	struct server_local server;
	init_server(&server, WORLD_B);
	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(server.death_drop_journal && server.death_drop_journal->count == 1);
	uint32_t tx = server.death_drop_journal->transaction_id;
	/* Chunk checkpoint fails -> journal stays PREPARED, drops NOT on disk, so
	 * the recovery below must genuinely REPLAY (and therefore must sync). */
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
	assert(!server_local_test_death_world_reload(&server));
	discard_server(&server);

	string_t name;
	string_init_set_str(name, WORLD_B);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	/* The replay's region fsync refuses -> recovery must refuse WHOLE, keeping
	 * the journal. FAIL-BEFORE: without the barrier in recover_chunk the armed
	 * failure is never consulted, recovery SUCCEEDS, and this assert fires. */
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_SYNC);
	assert(!death_drop_recover_if_needed(name, &level));
	struct death_drop_journal disk;
	assert(death_drop_journal_load(name, &disk) == DEATH_DROP_JOURNAL_LOAD_OK
		   && "the journal must SURVIVE a refused recovery");
	level_archive_destroy(&level);
	/* Healthy retry completes and applies exactly once. */
	assert(level_archive_create(&level, name));
	assert(death_drop_recover_if_needed(name, &level));
	level_archive_destroy(&level);
	string_clear(name);
	assert_level_inventory_empty(WORLD_B);
	bool seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_B, tx, seen) == 1);
	assert(seen[0]);
	puts("  death journal: recovery region-sync failure keeps the journal,"
		 " retry applies once");
}

/* A region handle can surface a deferred write error only when fclose runs.
   Once that close was attempted the handle is gone, so boot recovery cannot
   treat the write as committed and continue to the irreversible Player update
   and journal removal. The real close still executes under the test hook; only
   its reported result is forced to fail. */
static void test_recovery_close_gates_player_and_journal_commit(void) {
	remove_world_files(WORLD_O);
	make_world_dirs(WORLD_O);
	create_level_with_hotbar(WORLD_O, ITEM_BREAD);
	struct server_local server;
	init_server(&server, WORLD_O);
	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(server.death_drop_journal && server.death_drop_journal->count == 1);
	uint32_t tx = server.death_drop_journal->transaction_id;

	/* Leave PREPARED as the sole recovery authority and force boot recovery to
	 * perform the region rewrite whose close result gates the later commits. */
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
	assert(!server_local_test_death_world_reload(&server));
	discard_server(&server);

	string_t name;
	string_init_set_str(name, WORLD_O);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_CLOSE);
	assert(!death_drop_recover_if_needed(name, &level)
		   && "a lost close result must block boot-recovery commit");

	struct death_drop_journal disk;
	assert(death_drop_journal_load(name, &disk) == DEATH_DROP_JOURNAL_LOAD_OK
		   && "close failure must retain the only recovery journal");
	struct item_data items[INVENTORY_SIZE] = {{0}};
	struct inventory inventory = {.items = items, .capacity = INVENTORY_SIZE};
	assert(level_archive_read_inventory(&level, &inventory));
	assert(items[INVENTORY_SLOT_HOTBAR].id == ITEM_BREAD
		   && items[INVENTORY_SLOT_HOTBAR].count == 7
		   && "close failure must not publish the empty respawn inventory");
	enum world_dim dimension = WORLD_DIM_OVERWORLD;
	assert(level_archive_read_player(&level, NULL, NULL, NULL, &dimension));
	assert(dimension == WORLD_DIM_NETHER
		   && "close failure must not publish the respawn dimension");
	level_archive_destroy(&level);

	/* The first pass wrote canonical bytes but its close evidence was rejected.
	 * A retry sees changed=false; it must STILL cross a fresh sync barrier before
	 * publishing Player/removing the journal. This injection is fail-before for
	 * the tempting `if(changed) sync` shortcut. */
	assert(level_archive_create(&level, name));
	region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_SYNC);
	assert(!death_drop_recover_if_needed(name, &level));
	level_archive_destroy(&level);
	assert(death_drop_journal_load(name, &disk) == DEATH_DROP_JOURNAL_LOAD_OK);
	assert(level_archive_create(&level, name));
	assert(level_archive_read_inventory(&level, &inventory));
	assert(items[INVENTORY_SLOT_HOTBAR].id == ITEM_BREAD
		   && "canonical retry sync failure must still preserve Player");
	level_archive_destroy(&level);

	/* A fully healthy retry may now publish the player and remove the journal
	 * exactly once. */
	assert(level_archive_create(&level, name));
	assert(death_drop_recover_if_needed(name, &level));
	level_archive_destroy(&level);
	string_clear(name);
	assert_level_inventory_empty(WORLD_O);
	bool seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_O, tx, seen) == 1);
	assert(seen[0]);
	puts("  death journal: recovery close failure keeps Player+journal, retry"
		 " applies once");
}

static void test_empty_transaction(void) {
	remove_world_files(WORLD_C);
	make_world_dirs(WORLD_C);
	create_level_with_hotbar(WORLD_C, 0);
	struct server_local server;
	init_server(&server, WORLD_C);
	vec3 pos = {8.5F, 70.0F, 8.5F};
	uint32_t next_entity_id = server.next_entity_id;
	assert(server_local_spawn_item_batch(pos, NULL, 0, false, &server));
	assert(server.next_entity_id == next_entity_id);

	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(server.death_drop_active && server.death_drop_prepared_durable);
	assert(server.death_drop_journal && server.death_drop_journal->count == 0);
	assert(server_local_spawn_sourced_item_batch(
		pos, NULL, NULL, 0, false, false, &server));
	assert(server.death_drop_transfer_index == 0);
	assert(server_local_test_death_world_reload(&server));
	assert(!server.death_drop_active);
	assert(!server.death_drop_journal);
	discard_server(&server);
	assert_level_inventory_empty(WORLD_C);
	puts("  death journal: empty batches and empty transaction commit cleanly");
}

static void test_open_container_state_and_frozen_old_world(void) {
	remove_world_files(WORLD_D);
	make_world_dirs(WORLD_D);
	create_level_with_hotbar(WORLD_D, ITEM_APPLE);
	struct server_local server;
	init_server(&server, WORLD_D);
	const w_coord_t chest_x = 20, chest_y = 70, chest_z = 4;
	struct server_chunk* chest_chunk = NULL;
	assert(server_world_load_chunk(&server.worlds[0], 1, 0, &chest_chunk)
		   && chest_chunk);
	assert(server_world_set_block(
		&server.worlds[0], chest_x, chest_y, chest_z,
		(struct block_data) {.type = BLOCK_CHEST}));
	server.chest_pos[0]
		= (struct complex_block_pos) {chest_x, chest_y, chest_z};
	A1B_STAMP(&server, chest, 0, &server.worlds[0]);
	server.chest_items[0][0]
		= (struct item_data) {.id = ITEM_BREAD, .count = 4};
	/* Seed the old flat archive too, plus an unrelated row in the same chunk.
	 * Raw recovery must clear only the journal-owned slot and preserve the other
	 * migration owner byte-for-byte. */
	server.chest_pos[1]
		= (struct complex_block_pos) {chest_x + 1, chest_y, chest_z};
	A1B_STAMP(&server, chest, 1, &server.worlds[0]);
	server.chest_items[1][0]
		= (struct item_data) {.id = ITEM_STICK, .durability = 7, .count = 2};
	assert(chest_archive_write(
		server.chest_pos, server.chest_items[0], server.level_name,
		WORLD_DIM_NETHER));
	assert(server_local_open_inventory(
		&server, &inventory_logic_chest, CHEST_SIZE, chest_x, chest_y,
		chest_z, WINDOWC_CHEST, WINDOW_TYPE_CHEST));
	struct inventory* chest = server.player.active_inventory;
	assert(chest && chest != &server.player.inventory);
	/* Move the bread from the chest into the carried hotbar without closing.
	 * The disk still owns the old chest stack when PREPARED is written. */
	inventory_clear_slot(chest, CHEST_SLOT_STORAGE);
	inventory_set_slot(
		chest, CHEST_SLOT_HOTBAR + 1,
		(struct item_data) {.id = ITEM_BREAD, .count = 4});
	server.player.x = 15.99; /* a single item tick would cross chunk 0 -> 1 */
	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(server.death_drop_journal && server.death_drop_journal->count == 2);
	assert(server.death_drop_journal->container_count == CHEST_SIZE_STORAGE);
	uint32_t tx = server.death_drop_journal->transaction_id;

	size_t tagged = 0;
	dict_entity_it_t it;
	dict_entity_it(it, server.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* entity = dict_entity_ref(it)->value;
		if(entity && entity->type == ENTITY_ITEM
		   && entity->data.item.death_transaction == tx) {
			assert(entity->pos[0] == 15.99F);
			entity->vel[0] = 1.0F;
			tagged++;
		}
		dict_entity_next(it);
	}
	assert(tagged == 2);
	assert(server_local_test_tick_entities(&server));
	dict_entity_it(it, server.entities);
	while(!dict_entity_end_p(it)) {
		struct entity* entity = dict_entity_ref(it)->value;
		if(entity && entity->type == ENTITY_ITEM
		   && entity->data.item.death_transaction == tx)
			assert(entity->pos[0] == 15.99F);
		dict_entity_next(it);
	}

	/* Crash before the post-close region checkpoint. Recovery must both add the
	 * two drops and remove the stale bread from the on-disk chest. */
	discard_server(&server);
	string_t name;
	string_init_set_str(name, WORLD_D);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	death_drop_recovery_test_fail_after_chunks(1);
	assert(!death_drop_recover_if_needed(name, &level));
	level_archive_discard(&level);
	/* Chunk 0 committed its drop records before the injected crash; chunk 1's
	 * stale chest state is still owned by PREPARED and completes on retry. */
	bool first_seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_D, tx, first_seen) == 2);
	assert(level_archive_create(&level, name));
	assert(death_drop_recover_if_needed(name, &level));
	level_archive_destroy(&level);
	string_clear(name);
	assert_level_inventory_empty(WORLD_D);
	bool seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_D, tx, seen) == 2);
	assert(seen[0] && seen[1]);
	struct item_data old_chest = read_chest_slot(
		WORLD_D, chest_x, chest_y, chest_z, 0);
	assert(old_chest.id == 0);
	static struct complex_block_pos legacy_pos[MAX_CHESTS];
	static struct item_data legacy_items[MAX_CHESTS][MAX_CHEST_SLOTS];
	string_init_set_str(name, WORLD_D);
	chest_archive_read(
		legacy_pos, legacy_items[0], name, WORLD_DIM_NETHER);
	string_clear(name);
	bool found_cleared = false, found_unrelated = false;
	for(int i = 0; i < MAX_CHESTS; i++) {
		if(legacy_pos[i].x == chest_x && legacy_pos[i].y == chest_y
		   && legacy_pos[i].z == chest_z) {
			found_cleared = true;
			assert(legacy_items[i][0].id == 0);
		}
		if(legacy_pos[i].x == chest_x + 1 && legacy_pos[i].y == chest_y
		   && legacy_pos[i].z == chest_z) {
			found_unrelated = true;
			assert(legacy_items[i][0].id == ITEM_STICK
				   && legacy_items[i][0].durability == 7
				   && legacy_items[i][0].count == 2);
		}
	}
	assert(found_cleared && found_unrelated);
	puts("  death journal: live chest edits recover and old-world drops stay frozen");
}

static void test_journal_write_rotation_failures(void) {
	const enum death_drop_journal_test_fail_stage stages[] = {
		DEATH_DROP_JOURNAL_TEST_FAIL_WRITE,
		DEATH_DROP_JOURNAL_TEST_FAIL_RENAME,
	};
	for(size_t stage = 0; stage < sizeof(stages) / sizeof(stages[0]); stage++)
		for(unsigned occurrence = 1; occurrence <= 2; occurrence++) {
			remove_world_files(WORLD_E);
			make_world_dirs(WORLD_E);
			create_level_with_hotbar(WORLD_E, ITEM_APPLE);
			struct server_local server;
			init_server(&server, WORLD_E);
			death_drop_journal_test_fail_after(stages[stage], occurrence);
			/* Deviation-#18 split: the lethal hit itself now always commits the
			 * dead state; the journal begin failure surfaces in the RESPAWN
			 * request instead, which stays retryable -- the player simply
			 * remains dead. No owner moved either way. */
			assert(server_local_set_player_health(&server, 0));
			assert(!server_local_respawn_request(&server));
			assert(server.player.health <= 0);
			assert(!server.death_drop_journal);
			struct item_data item;
			assert(inventory_get_slot(
				&server.player.inventory, INVENTORY_SLOT_HOTBAR, &item));
			assert(item.id == ITEM_APPLE && item.count == 7);
			string_t name;
			string_init_set_str(name, WORLD_E);
			struct death_drop_journal journal;
			assert(death_drop_journal_load(name, &journal)
				   == DEATH_DROP_JOURNAL_LOAD_NONE);
			string_clear(name);
			discard_server(&server);
		}
	puts("  death journal: PREPARING/PREPARED write and rename failures retain owners");
}

static void test_partial_cleanup_does_not_resurrect_amended_sidecar(void) {
	struct death_drop_journal base = {
		.phase = DEATH_DROP_JOURNAL_PREPARED,
		.source_dimension = WORLD_DIM_NETHER,
		.transaction_id = UINT32_C(0x1730C1EA),
		.final_position = {8.5F, 70.0F, 8.5F},
		.final_dimension = WORLD_DIM_OVERWORLD,
		.final_health = MAX_PLAYER_HEALTH,
		.final_player_spawn_known = true,
		.final_has_player_spawn = true,
		.final_player_spawn = {-12, 71, 34},
		.count = 1,
	};
	base.records[0] = (struct death_drop_record) {
		.item = {.id = ITEM_APPLE, .count = 1},
		.pos = {8.5F, 70.0F, 8.5F},
		.provenance = {
			.kind = DEATH_DROP_OWNER_PLAYER_SLOT,
			.window = WINDOWC_INVENTORY,
			.slot = INVENTORY_SLOT_HOTBAR,
		},
	};
	struct death_drop_journal amended = base;
	amended.count = 2;
	amended.records[1] = (struct death_drop_record) {
		.item = {.id = ITEM_BREAD, .durability = 9, .count = 2},
		.pos = {8.5F, 70.0F, 8.5F},
		.provenance = {
			.kind = DEATH_DROP_OWNER_PLAYER_SLOT,
			.window = WINDOWC_INVENTORY,
			.slot = INVENTORY_SLOT_HOTBAR + 1,
		},
	};

	char current[256], fresh[256], old[256];
	snprintf(current, sizeof(current), "%s/death_drops.dat", WORLD_K);
	snprintf(fresh, sizeof(fresh), "%s/death_drops.dat_new", WORLD_K);
	snprintf(old, sizeof(old), "%s/death_drops.dat_old", WORLD_K);
	for(unsigned failed_removal = 2; failed_removal <= 3;
		failed_removal++) {
		remove_world_files(WORLD_K);
		make_world_dirs(WORLD_K);
		string_t name;
		string_init_set_str(name, WORLD_K);

		if(failed_removal == 2) {
			/* current=base, _new=amended, _old=base. */
			assert(death_drop_journal_store(name, &base)
				   == DEATH_DROP_JOURNAL_STORE_DURABLE);
			assert(death_drop_journal_store(name, &amended)
				   == DEATH_DROP_JOURNAL_STORE_DURABLE);
			assert(rename(current, fresh) == 0);
			assert(rename(old, current) == 0);
			copy_file(current, old);
		} else {
			/* current=base, _new=base, _old=amended. */
			assert(death_drop_journal_store(name, &amended)
				   == DEATH_DROP_JOURNAL_STORE_DURABLE);
			assert(death_drop_journal_store(name, &base)
				   == DEATH_DROP_JOURNAL_STORE_DURABLE);
			copy_file(current, fresh);
		}
		assert(file_exists(current) && file_exists(fresh) && file_exists(old));

		death_drop_journal_test_fail_after(
			DEATH_DROP_JOURNAL_TEST_FAIL_REMOVE_PATH, failed_removal);
		assert(!death_drop_journal_remove(name));
		assert(file_exists(current));
		struct death_drop_journal restarted;
		assert(death_drop_journal_load(name, &restarted)
			   == DEATH_DROP_JOURNAL_LOAD_OK);
		assert(restarted.transaction_id == base.transaction_id);
		assert(restarted.count == base.count);
		assert(restarted.final_player_spawn_known
			   && restarted.final_has_player_spawn);
		assert(restarted.final_player_spawn[0] == -12
			   && restarted.final_player_spawn[1] == 71
			   && restarted.final_player_spawn[2] == 34);
		assert(same_item(restarted.records[0].item, base.records[0].item));
		assert(death_drop_journal_remove(name));
		assert(death_drop_journal_load(name, &restarted)
			   == DEATH_DROP_JOURNAL_LOAD_NONE);
		string_clear(name);
	}
	puts("  death journal: partial cleanup cannot resurrect amended sidecars");
}

static void test_large_multi_callback_plan(void) {
	remove_world_files(WORLD_F);
	make_world_dirs(WORLD_F);
	create_level_with_hotbar(WORLD_F, ITEM_APPLE);
	struct server_local server;
	init_server(&server, WORLD_F);
	struct item_data stack = {.id = ITEM_APPLE, .count = 1};
	for(size_t slot = INVENTORY_SLOT_CRAFTING;
		slot < INVENTORY_SLOT_CRAFTING + INVENTORY_SIZE_CRAFTING; slot++)
		inventory_set_slot(&server.player.inventory, slot, stack);
	for(size_t slot = INVENTORY_SLOT_ARMOR; slot < INVENTORY_SIZE; slot++)
		inventory_set_slot(&server.player.inventory, slot, stack);
	inventory_set_picked_item(&server.player.inventory, stack);

	assert(server_world_set_block(
		&server.worlds[0], 10, 70, 10,
		(struct block_data) {.type = BLOCK_IRON_CHEST}));
	server.chest_pos[0] = (struct complex_block_pos) {10, 70, 10};
	A1B_STAMP(&server, chest, 0, &server.worlds[0]);
	A1B_BIND(&server.player, &server.worlds[0]);
	struct inventory* iron = calloc(1, sizeof(*iron));
	assert(iron && inventory_create(
		iron, &inventory_logic_iron_chest, &server, &server.player, IRON_CHEST_SIZE,
		10, 70, 10));
	for(size_t slot = IRON_CHEST_SLOT_STORAGE;
		slot < IRON_CHEST_SLOT_STORAGE + IRON_CHEST_SIZE_STORAGE; slot++)
		inventory_set_slot(iron, slot, stack);
	for(size_t slot = IRON_CHEST_SLOT_MAIN;
		slot < IRON_CHEST_SLOT_MAIN + INVENTORY_SIZE_MAIN; slot++)
		inventory_set_slot(iron, slot, stack);
	for(size_t slot = IRON_CHEST_SLOT_HOTBAR;
		slot < IRON_CHEST_SLOT_HOTBAR + INVENTORY_SIZE_HOTBAR; slot++)
		inventory_set_slot(iron, slot, stack);
	inventory_set_picked_item(iron, stack);
	/* Reproduce an orphaned live window without running the block-removal spill:
	 * the close callback sees no authoritative row and owns all 54 storage slots. */
	server.chest_pos[0].y = -1;
	server.player.active_inventory = iron;

	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	/* 4 player-craft + 1 player cursor + 54 orphan storage + 1 active
	 * cursor + 4 armor + 36 carried main/hotbar. The 55-record orphan
	 * callback arrives after a 100-record plan and must be checked against
	 * remaining transfer ownership, not unused journal capacity. */
	assert(server.death_drop_journal && server.death_drop_journal->count == 100);
	assert(server.death_drop_transfer_index == 100);
	uint32_t tx = server.death_drop_journal->transaction_id;
	discard_server(&server);

	string_t name;
	string_init_set_str(name, WORLD_F);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	assert(death_drop_recover_if_needed(name, &level));
	level_archive_destroy(&level);
	string_clear(name);
	bool seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_F, tx, seen) == 100);
	for(size_t i = 0; i < 100; i++) assert(seen[i]);
	assert_level_inventory_empty(WORLD_F);
	puts("  death journal: 100-record multi-callback plan transfers completely");
}

static void test_live_plan_divergence_uses_disk_recovery(void) {
	remove_world_files(WORLD_G);
	make_world_dirs(WORLD_G);
	create_level_with_hotbar(WORLD_G, ITEM_APPLE);
	struct server_local server;
	init_server(&server, WORLD_G);
	inventory_set_slot(
		&server.player.inventory, INVENTORY_SLOT_CRAFTING,
		(struct item_data) {.id = BLOCK_STONE, .count = 2});
	server_local_test_diverge_next_death_close();
	server.paused = true;
	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(!server.paused);
	assert(server.death_drop_active
		   && server.death_drop_recover_from_disk
		   && server.player.has_pos);
	assert(server.death_drop_journal);
	uint32_t tx = server.death_drop_journal->transaction_id;
	assert(server_local_test_death_world_reload(&server));
	assert(server.player.has_pos
		   && server.worlds[0].dimension == WORLD_DIM_OVERWORLD);
	assert(!server.death_drop_active
		   && !server.death_drop_recover_from_disk);
	assert(!server.death_drop_journal);
	discard_server(&server);

	assert_level_inventory_empty(WORLD_G);
	bool seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_G, tx, seen) == 2);
	assert(seen[0] && seen[1]);
	puts("  death journal: live-plan divergence abandons source and recovers disk plan");
}

static void test_published_unsynced_stays_behind_loading_barrier(void) {
	remove_world_files(WORLD_CONTAINER_AMEND);
	make_world_dirs(WORLD_CONTAINER_AMEND);
	create_level_with_hotbar(WORLD_CONTAINER_AMEND, ITEM_APPLE);
	struct server_local server;
	init_server(&server, WORLD_CONTAINER_AMEND);
	struct inventory* live_inventory = calloc(1, sizeof(*live_inventory));
	assert(live_inventory && inventory_create(
		live_inventory, &inventory_logic_crafting, &server, &server.player,
		CRAFTING_SIZE, 2, 70, 2));
	inventory_set_slot(
		live_inventory, CRAFTING_SLOT_INPUT,
		(struct item_data) {.id = BLOCK_STONE, .count = 2});
	server.player.active_inventory = live_inventory;

	/* The second committed-name sync is PREPARED (the first is PREPARING).
	 * Keep the independent remove failure armed until forward recovery reaches
	 * its cleanup boundary. This is the exact crash window where a bool result
	 * previously restored live play while a valid PREPARED current survived. */
	death_drop_journal_test_fail_after(
		DEATH_DROP_JOURNAL_TEST_FAIL_COMMITTED_SYNC, 2);
	death_drop_journal_test_fail_next(DEATH_DROP_JOURNAL_TEST_FAIL_REMOVE);
	server.paused = true;
	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(!server.paused);
	assert(server.player.health == 0 && server.player.has_pos);
	assert(server.world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
	assert(server.death_world_evict && server.death_drop_active);
	assert(!server.death_drop_prepared_durable
		   && server.death_drop_recover_from_disk);
	assert(server.death_drop_journal
		   && server.death_drop_journal->phase == DEATH_DROP_JOURNAL_PREPARED);
	assert(server.death_drop_journal->count == 2);
	struct death_drop_journal* live_journal = server.death_drop_journal;
	uint32_t tx = server.death_drop_journal->transaction_id;

	/* No source callback, slot clear, or EntityItem publication is permitted
	 * before a retry proves PREPARED durable. The exact active-window pointer is
	 * retained too; only the later abandon path may destroy an open view. */
	assert(server.player.active_inventory == live_inventory);
	struct item_data live_item;
	assert(inventory_get_slot(
		&server.player.inventory, INVENTORY_SLOT_HOTBAR, &live_item));
	assert(live_item.id == ITEM_APPLE && live_item.count == 7);
	assert(inventory_get_slot(
		live_inventory, CRAFTING_SLOT_INPUT, &live_item));
	assert(live_item.id == BLOCK_STONE && live_item.count == 2);
	dict_entity_it_t it;
	dict_entity_it(it, server.entities);
	while(!dict_entity_end_p(it)) {
		const struct entity* entity = dict_entity_cref(it)->value;
		assert(!entity || entity->type != ENTITY_ITEM
			   || entity->data.item.death_transaction != tx);
		dict_entity_next(it);
	}

	/* A persistent committed-name sync failure retains both concurrent heap
	 * owners and every source stack behind the loading barrier for another
	 * bounded retry. */
	death_drop_journal_test_fail_next(
		DEATH_DROP_JOURNAL_TEST_FAIL_COMMITTED_SYNC);
	assert(!server_local_test_death_world_reload(&server));
	assert(server.player.has_pos && server.death_world_evict);
	assert(server.death_world_retry_delay > 0);
	assert(server.player.active_inventory == live_inventory);
	assert(server.death_drop_journal == live_journal);
	assert(server.death_drop_active && !server.death_drop_prepared_durable
		   && server.death_drop_recover_from_disk);
	assert(inventory_get_slot(
		live_inventory, CRAFTING_SLOT_INPUT, &live_item));
	assert(live_item.id == BLOCK_STONE && live_item.count == 2);

	/* The next retry re-stores/syncs PREPARED before abandoning the live source.
	 * Recovery then completes forward, but the injected journal-remove failure
	 * must keep the client positionless behind the same loading/retry barrier. */
	assert(!server_local_test_death_world_reload(&server));
	assert(!server.player.has_pos && server.death_world_evict);
	assert(server.death_world_retry_delay > 0);
	assert(server.world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
	assert(!server.death_drop_active && !server.death_drop_journal);
	bool first_seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_CONTAINER_AMEND, tx, first_seen) == 2);
	assert(first_seen[0] && first_seen[1]);

	/* Replaying the still-PREPARED journal is idempotent: the second recovery
	 * removes it, opens the Overworld exactly once, and never duplicates the
	 * already committed transaction entity. */
	assert(server_local_test_death_world_reload(&server));
	assert(server.player.has_pos
		   && server.worlds[0].dimension == WORLD_DIM_OVERWORLD);
	assert(!server.death_drop_active
		   && !server.death_drop_recover_from_disk);
	assert(!server.death_drop_journal);
	discard_server(&server);
	assert_level_inventory_empty(WORLD_CONTAINER_AMEND);
	bool final_seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_CONTAINER_AMEND, tx, final_seen) == 2);
	assert(final_seen[0] && final_seen[1]);
	puts("  death journal: published-unsynced PREPARED stays loading and completes once");
}

/* Power-cut image of a whole world directory: what the card holds right now.
 * Bytes still in a live process's FILE* buffers are NOT copied (they are lost
 * by a power cut too); bytes already handed to the OS are, which only makes
 * the image MORE favourable than a real cut, never less. */
static void copy_world_tree(const char* from, const char* to, int depth) {
	assert(depth < 8);
	MKDIR(to);
	DIR* d = opendir(from);
	assert(d);
	struct dirent* e;
	while((e = readdir(d))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char src[640], dst[640];
		snprintf(src, sizeof src, "%.300s/%.300s", from, e->d_name);
		snprintf(dst, sizeof dst, "%.300s/%.300s", to, e->d_name);
		if(test_fixture_is_dir(from, e->d_name))
			copy_world_tree(src, dst, depth + 1);
		else
			copy_file(src, dst);
	}
	closedir(d);
}

/* DEATH-PREPARED-DURABLE (ASTRA-G9-R19-IO death invariant: every required
   base durable BEFORE PREPARED). In production the save batch is open on
   almost every tick (it closes once per SAVE_BATCH_MAX_TICKS), so the death
   begin path's base checkpoint lands in an OPEN batch: the payload is in the
   file but the region index that names it is only published at the next
   batch end. Before the fix, DEFAULT mode (experimental.g9_save_io_int=0)
   stored a durable PREPARED anyway -- only the deferred worker took the
   region barrier. A power cut in that window leaves a durable PREPARED whose
   base chunk the card does not have: boot recovery can never complete
   forward (the journal is stuck, the drops are never materialized, the
   player's items are gone from both the world and the durable inventory's
   future). FAIL-BEFORE: the image recovery below returns false. */
static void test_open_batch_base_durable_before_prepared(void) {
	remove_world_files(WORLD_BATCH_LIVE);
	make_world_dirs(WORLD_BATCH_LIVE);
	create_level_with_hotbar(WORLD_BATCH_LIVE, ITEM_APPLE);
	struct server_local server;
	init_server(&server, WORLD_BATCH_LIVE);
	inventory_set_slot(
		&server.player.inventory, INVENTORY_SLOT_CRAFTING,
		(struct item_data) {.id = BLOCK_STONE, .count = 2});
	/* The production tick shape: the batch is open while the death runs. */
	server_world_save_batch_begin(&server.worlds[0]);
	assert(server.worlds[0].save_batch_open);
	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(server.death_drop_active && server.death_drop_prepared_durable);
	assert(server.death_drop_journal && server.death_drop_journal->count == 2);
	uint32_t tx = server.death_drop_journal->transaction_id;
	string_t name;
	string_init_set_str(name, WORLD_BATCH_LIVE);
	struct death_drop_journal disk;
	assert(death_drop_journal_load(name, &disk) == DEATH_DROP_JOURNAL_LOAD_OK);
	assert(disk.phase == DEATH_DROP_JOURNAL_PREPARED);
	string_clear(name);

	/* Power cut NOW: PREPARED is durable, the batch has not ended. */
	test_fixture_rm_rf(WORLD_BATCH_CUT);
	copy_world_tree(WORLD_BATCH_LIVE, WORLD_BATCH_CUT, 0);

	/* Boot recovery of the image must complete forward, exactly once. */
	string_init_set_str(name, WORLD_BATCH_CUT);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	bool recovered = death_drop_recover_if_needed(name, &level);
	if(!recovered)
		fprintf(stderr,
				"  [RED] power-cut image after a durable PREPARED cannot "
				"recover: the open-batch base chunk was never durable\n");
	assert(recovered && "PREPARED must never be durable before its base");
	level_archive_destroy(&level);
	assert(death_drop_journal_load(name, &disk) == DEATH_DROP_JOURNAL_LOAD_NONE);
	string_clear(name);
	assert_level_inventory_empty(WORLD_BATCH_CUT);
	bool seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_BATCH_CUT, tx, seen) == 2);
	assert(seen[0] && seen[1]);

	/* The live session (no cut) still completes once through its own reload. */
	assert(server_world_save_batch_end(&server.worlds[0]));
	assert(server_local_test_death_world_reload(&server));
	assert(!server.death_drop_active && !server.death_drop_journal);
	discard_server(&server);
	assert_level_inventory_empty(WORLD_BATCH_LIVE);
	bool live_seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	assert(read_transaction_entities(WORLD_BATCH_LIVE, tx, live_seen) == 2);
	assert(live_seen[0] && live_seen[1]);
	test_fixture_rm_rf(WORLD_BATCH_CUT);
	puts("  death journal: open-batch base is durable before PREPARED"
		 " (power-cut image recovers once)");
}

static void test_lethal_explosion_destroys_live_chest_once(void) {
	remove_world_files(WORLD_I);
	make_world_dirs(WORLD_I);
	create_level_with_hotbar(WORLD_I, ITEM_BREAD);
	struct server_local server;
	init_server(&server, WORLD_I);
	const w_coord_t x = 8, y = 70, z = 8;
	const struct item_data live_chest_stack
		= {.id = ITEM_BREAD, .count = 7};
	const struct item_data player_stack
		= {.id = ITEM_DYE, .durability = 4, .count = 2};
	assert(server_world_set_block(
		&server.worlds[0], x, y, z, (struct block_data) {.type = BLOCK_CHEST}));
	server.chest_pos[0] = (struct complex_block_pos) {x, y, z};
	A1B_STAMP(&server, chest, 0, &server.worlds[0]);
	server.chest_items[0][0] = player_stack;
	assert(server_local_open_inventory(
		&server, &inventory_logic_chest, CHEST_SIZE, x, y, z,
		WINDOWC_CHEST, WINDOW_TYPE_CHEST));
	struct inventory* live = server.player.active_inventory;
	assert(live && live != &server.player.inventory);
	/* Swap the two stacks only in the live window. The disk still has dye in the
	 * chest and bread in the player hotbar when Explosion.doExplosionA delivers
	 * the lethal hit. Death must preserve both sides of the concurrent edit before
	 * doExplosionB removes the block. */
	inventory_set_slot(live, CHEST_SLOT_STORAGE, live_chest_stack);
	inventory_set_slot(live, CHEST_SLOT_HOTBAR, player_stack);
	server.player.health = 1;
	server_world_explode(
		&server.worlds[0], (vec3) {x + 0.5F, y + 0.5F, z + 0.5F}, 4.0F);
	/* Deviation-#18 split: the explosion's lethal hit commits only the DEAD
	 * state; the chest window faithfully stays open through the dwell
	 * (container-open dwell) and the close/journal transaction runs at the
	 * explicit request. */
	assert(server.player.health <= 0);
	assert(server.player.active_inventory != &server.player.inventory);
	assert(server_local_respawn_request(&server));

	struct block_data block;
	assert(server_world_get_block(&server.worlds[0], x, y, z, &block));
	assert(block.type == BLOCK_AIR);
	assert(server.player.active_inventory == &server.player.inventory);
	assert(server.player.health == MAX_PLAYER_HEALTH);
	assert(server.death_drop_active && server.death_drop_prepared_durable);
	assert(server.death_drop_journal);
	/* Deviation-#18 ordering: doExplosionB destroyed the chest during the
	 * dead dwell; its live-edited stack spilled right then through the
	 * ordinary block-destroy commit (an UNTAGGED world drop). The respawn's
	 * journal therefore carries only the player stack. */
	assert(server.death_drop_journal->container_count == 0);
	assert(server.death_drop_journal->count == 1);
	assert(same_item(
		server.death_drop_journal->records[0].item, player_stack));
	assert(server.death_drop_journal->records[0].provenance.kind
		   == DEATH_DROP_OWNER_PLAYER_SLOT);
	assert(server.death_drop_transfer_index == 1);
	assert(server.chest_pos[0].y == -1);
	uint32_t tx = server.death_drop_journal->transaction_id;
	size_t runtime_transaction_count = 0;
	size_t runtime_player_count = 0;
	size_t runtime_live_chest_count = 0;
	dict_entity_it_t it;
	dict_entity_it(it, server.entities);
	while(!dict_entity_end_p(it)) {
		const struct entity* entity = dict_entity_cref(it)->value;
		if(entity && entity->type == ENTITY_ITEM) {
			if(same_item(entity->data.item.item, player_stack))
				runtime_player_count++;
			if(same_item(entity->data.item.item, live_chest_stack))
				runtime_live_chest_count++;
			if(entity->data.item.death_transaction == tx)
				runtime_transaction_count++;
		}
		dict_entity_next(it);
	}
	/* One tagged (journaled player stack) + one untagged (block-destroy
	 * spill) -- exactly-once for both, the property this golden pins. */
	assert(runtime_transaction_count == 1);
	assert(runtime_player_count == 1);
	assert(runtime_live_chest_count == 1);

	/* Prove both the live checkpoint and startup recovery path. The source
	 * region succeeds (including the destroyed owner and its two tagged stacks),
	 * then an injected journal-remove failure models power loss at the last step. */
	death_drop_journal_test_fail_next(DEATH_DROP_JOURNAL_TEST_FAIL_REMOVE);
	assert(!server_local_test_death_world_reload(&server));
	assert(server.death_drop_active && server.player.has_pos);
	discard_server(&server);
	assert_destroyed_chest_drop_state(
		WORLD_I, x, y, z, tx, player_stack, live_chest_stack);

	string_t name;
	string_init_set_str(name, WORLD_I);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	assert(death_drop_recover_if_needed(name, &level));
	level_archive_destroy(&level);
	string_clear(name);
	assert_level_inventory_empty(WORLD_I);
	assert_destroyed_chest_drop_state(
		WORLD_I, x, y, z, tx, player_stack, live_chest_stack);
	puts("  death journal: lethal explosion destroys live-edited chest exactly once");
}

static void test_failed_container_amendment_restores_math_random(void) {
	remove_world_files(WORLD_J);
	make_world_dirs(WORLD_J);
	create_level_with_hotbar(WORLD_J, ITEM_APPLE);
	struct server_local server;
	init_server(&server, WORLD_J);
	const w_coord_t x = 8, y = 70, z = 8;
	const struct item_data chest_stack
		= {.id = ITEM_BREAD, .count = 7};
	assert(server_world_set_block(
		&server.worlds[0], x, y, z, (struct block_data) {.type = BLOCK_CHEST}));
	server.chest_pos[0] = (struct complex_block_pos) {x, y, z};
	A1B_STAMP(&server, chest, 0, &server.worlds[0]);
	server.chest_items[0][0] = chest_stack;
	assert(server_local_open_inventory(
		&server, &inventory_logic_chest, CHEST_SIZE, x, y, z,
		WINDOWC_CHEST, WINDOW_TYPE_CHEST));

	/* First make the live chest slots part of a durable PREPARED death plan.
	 * The later block-removal callback must replace that owner with EntityItem
	 * records transactionally. */
	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(server.death_drop_active && server.death_drop_prepared_durable);
	assert(server.death_drop_journal
		   && server.death_drop_journal->phase
			  == DEATH_DROP_JOURNAL_PREPARED);
	assert(server.death_drop_journal->container_count
		   == CHEST_SIZE_STORAGE);
	assert(server.chest_items[0][0].id == chest_stack.id
		   && server.chest_items[0][0].count == chest_stack.count);

	/* EntityItem construction consumes four values from java.lang.Math's single
	 * process-global Random even though the journal stores only the resulting
	 * velocity. An unpublished amendment owns none of those tentative draws. */
	server_local_test_seed_math_random(INT64_C(0x123456789AB));
	struct server_local_math_random_state math_before, math_after;
	server_local_math_random_capture(&math_before);
	struct random_gen container_rng_before = server.container_drop_rng;
	size_t record_count_before = server.death_drop_journal->count;
	size_t container_count_before
		= server.death_drop_journal->container_count;
	struct death_drop_container_slot slots_before[CHEST_SIZE_STORAGE];
	memcpy(slots_before, server.death_drop_journal->container_slots,
		   sizeof(slots_before));

	death_drop_journal_test_fail_next(DEATH_DROP_JOURNAL_TEST_FAIL_WRITE);
	assert(!server_local_spawn_destroyed_container_items(
		&server, x, y, z, server.chest_items[0], CHEST_SIZE_STORAGE));
	server_local_math_random_capture(&math_after);

	assert(server.death_drop_journal->count == record_count_before);
	assert(server.death_drop_journal->container_count
		   == container_count_before);
	assert(memcmp(slots_before, server.death_drop_journal->container_slots,
			  sizeof(slots_before)) == 0);
	assert(server.container_drop_rng.seed == container_rng_before.seed);
	assert(math_after.initialized == math_before.initialized
		   && math_after.rng.seed == math_before.rng.seed);
	discard_server(&server);
	puts("  death journal: failed container amendment restores exact Math.random state");
}

static void test_recovery_commits_personal_spawn_state(void) {
	remove_world_files(WORLD_L);
	make_world_dirs(WORLD_L);
	create_level_with_hotbar(WORLD_L, 0);
	string_t name;
	string_init_set_str(name, WORLD_L);
	struct death_drop_journal journal = {
		.phase = DEATH_DROP_JOURNAL_PREPARED,
		.transaction_id = UINT32_C(0x173B3D01),
		.source_dimension = WORLD_DIM_NETHER,
		.final_position = {12.5F, 72.72F, -7.5F},
		.final_dimension = WORLD_DIM_OVERWORLD,
		.final_health = MAX_PLAYER_HEALTH,
		.final_player_spawn_known = true,
		.final_has_player_spawn = true,
		.final_player_spawn = {14, 71, -8},
	};
	assert(death_drop_journal_store(name, &journal)
		   == DEATH_DROP_JOURNAL_STORE_DURABLE);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	assert(death_drop_recover_if_needed(name, &level));
	level_archive_destroy(&level);
	assert(level_archive_create(&level, name));
	bool present = false;
	int32_t x = 0, y = 0, z = 0;
	assert(level_archive_read_player_spawn(
		&level, &present, &x, &y, &z));
	assert(present && x == 14 && y == 71 && z == -8);
	level_archive_destroy(&level);

	/* Version 3 inserted the personal-spawn fields immediately before both
	 * variable-length payload tables. Exercise a genuinely non-empty v2 image so
	 * accepting its header cannot hide a 13-byte record/container offset bug. */
	journal.transaction_id++;
	journal.count = 1;
	journal.records[0] = (struct death_drop_record) {
		.item = {.id = ITEM_APPLE, .count = 2, .durability = 7},
		.pos = {9.25F, 70.5F, 10.75F},
		.vel = {0.125F, 0.25F, -0.5F},
		.provenance = {
			.kind = DEATH_DROP_OWNER_PLAYER_SLOT,
			.window = WINDOWC_INVENTORY,
			.slot = INVENTORY_SLOT_HOTBAR,
		},
	};
	journal.container_count = 1;
	journal.container_slots[0] = (struct death_drop_container_slot) {
		.item = {.id = ITEM_BREAD, .count = 3, .durability = 11},
		.window = WINDOWC_CHEST,
		.owner_slot = 2,
		.owner_x = 8,
		.owner_y = 70,
		.owner_z = 8,
	};
	assert(death_drop_journal_store(name, &journal)
		   == DEATH_DROP_JOURNAL_STORE_DURABLE);
	rewrite_current_journal_as_v2(WORLD_L);
	struct death_drop_journal legacy_payload;
	assert(death_drop_journal_load(name, &legacy_payload)
		   == DEATH_DROP_JOURNAL_LOAD_OK);
	assert(!legacy_payload.final_player_spawn_known);
	assert(legacy_payload.count == 1 && legacy_payload.container_count == 1);
	assert(legacy_payload.records[0].item.id == ITEM_APPLE
		   && legacy_payload.records[0].item.count == 2
		   && legacy_payload.records[0].item.durability == 7);
	assert(legacy_payload.records[0].pos[0] == 9.25F
		   && legacy_payload.records[0].pos[1] == 70.5F
		   && legacy_payload.records[0].pos[2] == 10.75F);
	assert(legacy_payload.records[0].vel[0] == 0.125F
		   && legacy_payload.records[0].vel[1] == 0.25F
		   && legacy_payload.records[0].vel[2] == -0.5F);
	assert(legacy_payload.records[0].provenance.kind
		   == DEATH_DROP_OWNER_PLAYER_SLOT);
	assert(legacy_payload.records[0].provenance.window == WINDOWC_INVENTORY
		   && legacy_payload.records[0].provenance.slot
			  == INVENTORY_SLOT_HOTBAR);
	assert(legacy_payload.container_slots[0].item.id == ITEM_BREAD
		   && legacy_payload.container_slots[0].item.count == 3
		   && legacy_payload.container_slots[0].item.durability == 11);
	assert(legacy_payload.container_slots[0].window == WINDOWC_CHEST
		   && legacy_payload.container_slots[0].owner_slot == 2
		   && legacy_payload.container_slots[0].owner_x == 8
		   && legacy_payload.container_slots[0].owner_y == 70
		   && legacy_payload.container_slots[0].owner_z == 8);
	assert(death_drop_journal_remove(name));
	journal.count = 0;
	journal.container_count = 0;

	/* A v2 journal predates the final personal-spawn state. It must preserve
	 * the level's existing valid Player.Spawn triple instead of clearing it. */
	journal.transaction_id++;
	assert(death_drop_journal_store(name, &journal)
		   == DEATH_DROP_JOURNAL_STORE_DURABLE);
	rewrite_current_journal_as_v2(WORLD_L);
	struct death_drop_journal legacy;
	assert(death_drop_journal_load(name, &legacy)
		   == DEATH_DROP_JOURNAL_LOAD_OK);
	assert(!legacy.final_player_spawn_known
		   && !legacy.final_has_player_spawn);
	assert(level_archive_create(&level, name));
	assert(death_drop_recover_if_needed(name, &level));
	level_archive_destroy(&level);
	assert(level_archive_create(&level, name));
	present = false;
	assert(level_archive_read_player_spawn(
		&level, &present, &x, &y, &z));
	assert(present && x == 14 && y == 71 && z == -8);
	level_archive_destroy(&level);

	journal.transaction_id++;
	journal.final_has_player_spawn = false;
	assert(death_drop_journal_store(name, &journal)
		   == DEATH_DROP_JOURNAL_STORE_DURABLE);
	assert(level_archive_create(&level, name));
	assert(death_drop_recover_if_needed(name, &level));
	level_archive_destroy(&level);
	assert(level_archive_create(&level, name));
	present = true;
	assert(level_archive_read_player_spawn(
		&level, &present, &x, &y, &z));
	assert(!present);
	level_archive_destroy(&level);

	/* A checksum-valid file is still untrusted save input. Unknown item ids must
	 * fail journal load before recovery can inject them into an inventory. */
	journal.transaction_id++;
	journal.final_has_player_spawn = false;
	journal.count = 1;
	journal.records[0] = (struct death_drop_record) {
		.item = {.id = ITEM_APPLE, .count = 1},
		.pos = {8.5F, 70.0F, 8.5F},
		.provenance = {
			.kind = DEATH_DROP_OWNER_PLAYER_SLOT,
			.window = WINDOWC_INVENTORY,
			.slot = INVENTORY_SLOT_HOTBAR,
		},
	};
	assert(death_drop_journal_store(name, &journal)
		   == DEATH_DROP_JOURNAL_STORE_DURABLE);
	rewrite_first_v3_record_id(WORLD_L, UINT16_MAX);
	struct death_drop_journal corrupt_item;
	enum death_drop_journal_load_result corrupt_loaded
		= death_drop_journal_load(name, &corrupt_item);
	assert(corrupt_loaded == DEATH_DROP_JOURNAL_LOAD_ERROR);
	assert(death_drop_journal_remove(name));
	string_clear(name);
	puts("  death journal: recovery preserves valid personal spawn and clears invalid");
}

static void test_nether_death_resolves_saved_overworld_bed(void) {
	remove_world_files(WORLD_M);
	make_world_dirs(WORLD_M);
	create_level_with_hotbar(WORLD_M, 0);
	string_t name;
	string_init_set_str(name, WORLD_M);
	struct server_local overworld = {0};
	server_world_create(
		&overworld.world, name, WORLD_DIM_OVERWORLD, 424242, true);
	overworld.worlds[0].session = &overworld; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&overworld.world, 0, 0, &chunk) && chunk);
	/* Saved head (8,70,8), Cave +Z metadata. First Beta-order candidate
	 * (7,70,7) has the sole normal floor. */
	assert(server_world_set_block_silent(
		&overworld.worlds[0], 8, 70, 8,
		(struct block_data) {.type = BLOCK_BED, .metadata = 8}));
	assert(server_world_set_block_silent(
		&overworld.worlds[0], 7, 69, 7,
		(struct block_data) {.type = BLOCK_STONE}));
	assert(server_world_save_chunk(&overworld.world, false, 0, 0));
	server_world_destroy(&overworld.world);

	struct server_local server;
	init_server(&server, WORLD_M);
	server.player.has_bed_spawn = true;
	server.player.bed_spawn_x = 8;
	server.player.bed_spawn_y = 70;
	server.player.bed_spawn_z = 8;
	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(server.player.has_bed_spawn);
	assert(server.player.x == 7.5 && server.player.z == 7.5);
	assert(fabs(server.player.y - (70 + PLAYER_EYE_HEIGHT + 0.1))
		   < 1.0e-6);
	assert(server.death_drop_journal);
	assert(server.death_drop_journal->final_player_spawn_known
		   && server.death_drop_journal->final_has_player_spawn);
	assert(server.death_drop_journal->final_position[0] == 7.5F
		   && server.death_drop_journal->final_position[2] == 7.5F);
	discard_server(&server);
	string_clear(name);
	puts("  death journal: Nether death resolves archived Overworld bed before PREPARED");
}

static void test_nether_death_clears_obstructed_overworld_bed(void) {
	remove_world_files(WORLD_N);
	make_world_dirs(WORLD_N);
	create_level_with_hotbar(WORLD_N, 0);
	string_t name;
	string_init_set_str(name, WORLD_N);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	assert(level_archive_write_player_spawn(&level, true, 8, 70, 8));
	assert(level_archive_save(&level));
	level_archive_destroy(&level);

	struct server_local overworld = {0};
	server_world_create(
		&overworld.world, name, WORLD_DIM_OVERWORLD, 424242, true);
	overworld.worlds[0].session = &overworld; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&overworld.world, 0, 0, &chunk) && chunk);
	/* Keep the exact stored head but fill both 3x3 passes with normal floors and
	 * solid headroom. This distinguishes a blocked bed from a removed one. */
	for(int x = 7; x <= 9; x++)
		for(int z = 6; z <= 9; z++) {
			assert(server_world_set_block_silent(
				&overworld.worlds[0], x, 69, z,
				(struct block_data) {.type = BLOCK_STONE}));
			assert(server_world_set_block_silent(
				&overworld.worlds[0], x, 70, z,
				(struct block_data) {.type = BLOCK_STONE}));
			assert(server_world_set_block_silent(
				&overworld.worlds[0], x, 71, z,
				(struct block_data) {.type = BLOCK_STONE}));
		}
	assert(server_world_set_block_silent(
		&overworld.worlds[0], 8, 70, 8,
		(struct block_data) {.type = BLOCK_BED, .metadata = 8}));
	assert(server_world_save_chunk(&overworld.world, false, 0, 0));
	server_world_destroy(&overworld.world);

	struct server_local server;
	init_server(&server, WORLD_N);
	server.player.has_bed_spawn = true;
	server.player.bed_spawn_x = 8;
	server.player.bed_spawn_y = 70;
	server.player.bed_spawn_z = 8;
	assert(server_local_set_player_health(&server, 0));
	assert(server_local_respawn_request(&server));
	assert(!server.player.has_bed_spawn);
	/* 2026-08-26 fork-audit fix (server_local_respawn_anti_suffocate): the
	 * invalid-bed fallback plan is world spawn (8,70,8) -- exactly the cell
	 * this fixture buried under BLOCK_BED (70) then BLOCK_STONE (71) to make
	 * the BED invalid in the first place. Beta's own push-up-while-colliding
	 * loop [740c583 ServerConfigurationManager.java:155-157] now applies here
	 * too: the fresh body is no longer dropped blind into that obstruction,
	 * it is pushed up to the first AIR/AIR gap above it (y=72, superflat's
	 * everything-above-y3-is-air). This assert used to pin the PRE-FIX
	 * embedded-in-stone placement (70 + EYE_HEIGHT) as if it were correct --
	 * it was the exact suffocation bug being fixed, not a real invariant. */
	assert(server.player.x == 8.5
		   && server.player.y == 72 + PLAYER_EYE_HEIGHT
		   && server.player.z == 8.5);
	assert(server.death_drop_journal
		   && server.death_drop_journal->final_player_spawn_known
		   && !server.death_drop_journal->final_has_player_spawn);
	assert(server_local_test_death_world_reload(&server));
	assert(server.worlds[0].dimension == WORLD_DIM_OVERWORLD
		   && server.player.dimension == WORLD_DIM_OVERWORLD
		   && !server.player.has_bed_spawn);
	discard_server(&server);

	assert(level_archive_create(&level, name));
	bool present = true;
	int32_t x = 0, y = 0, z = 0;
	assert(level_archive_read_player_spawn(
		&level, &present, &x, &y, &z));
	assert(!present);
	level_archive_destroy(&level);
	string_clear(name);
	puts("  death journal: obstructed archived bed clears durably on Overworld reload");
}

int main(void) {
	test_fixture_dir("test_death_drop_journal_a", s_fixture_WORLD_A, sizeof s_fixture_WORLD_A);
	test_fixture_dir("test_death_drop_journal_b", s_fixture_WORLD_B, sizeof s_fixture_WORLD_B);
	test_fixture_dir("test_death_drop_journal_c", s_fixture_WORLD_C, sizeof s_fixture_WORLD_C);
	test_fixture_dir("test_death_drop_journal_d", s_fixture_WORLD_D, sizeof s_fixture_WORLD_D);
	test_fixture_dir("test_death_drop_journal_e", s_fixture_WORLD_E, sizeof s_fixture_WORLD_E);
	test_fixture_dir("test_death_drop_journal_f", s_fixture_WORLD_F, sizeof s_fixture_WORLD_F);
	test_fixture_dir("test_death_drop_journal_g", s_fixture_WORLD_G, sizeof s_fixture_WORLD_G);
	test_fixture_dir("test_death_drop_journal_h", s_fixture_WORLD_CONTAINER_AMEND, sizeof s_fixture_WORLD_CONTAINER_AMEND);
	test_fixture_dir("test_death_drop_journal_i", s_fixture_WORLD_I, sizeof s_fixture_WORLD_I);
	test_fixture_dir("test_death_drop_journal_j", s_fixture_WORLD_J, sizeof s_fixture_WORLD_J);
	test_fixture_dir("test_death_drop_journal_k", s_fixture_WORLD_K, sizeof s_fixture_WORLD_K);
	test_fixture_dir("test_death_drop_journal_l", s_fixture_WORLD_L, sizeof s_fixture_WORLD_L);
	test_fixture_dir("test_death_drop_journal_m", s_fixture_WORLD_M, sizeof s_fixture_WORLD_M);
	test_fixture_dir("test_death_drop_journal_n", s_fixture_WORLD_N, sizeof s_fixture_WORLD_N);
	test_fixture_dir("test_death_drop_journal_o", s_fixture_WORLD_O, sizeof s_fixture_WORLD_O);
	test_fixture_dir("test_death_drop_journal_p", s_fixture_WORLD_DISPENSER_LIVE, sizeof s_fixture_WORLD_DISPENSER_LIVE);
	test_fixture_dir("test_death_drop_journal_q", s_fixture_WORLD_DISPENSER_REPLAY, sizeof s_fixture_WORLD_DISPENSER_REPLAY);
	test_fixture_dir("test_death_drop_journal_r", s_fixture_WORLD_BATCH_LIVE, sizeof s_fixture_WORLD_BATCH_LIVE);
	test_fixture_dir("test_death_drop_journal_s", s_fixture_WORLD_BATCH_CUT, sizeof s_fixture_WORLD_BATCH_CUT);
	assert(clin_init());
	assert(svin_init());
	blocks_init();
	items_init();
	struct thread drain;
	thread_create(&drain, drain_client, NULL, 64);
	test_crash_before_chunk_checkpoint();
	test_partial_region_and_level_before_remove();
	test_region_sync_gates_journal_removal();
	test_recovery_sync_gates_journal_removal();
	test_recovery_close_gates_player_and_journal_commit();
	test_empty_transaction();
	test_open_container_state_and_frozen_old_world();
	test_journal_write_rotation_failures();
	test_partial_cleanup_does_not_resurrect_amended_sidecar();
	test_large_multi_callback_plan();
	test_live_plan_divergence_uses_disk_recovery();
	test_published_unsynced_stays_behind_loading_barrier();
	test_open_batch_base_durable_before_prepared();
	test_lethal_explosion_destroys_live_chest_once();
	test_failed_container_amendment_restores_math_random();
	test_recovery_commits_personal_spawn_state();
	test_nether_death_resolves_saved_overworld_bed();
	test_nether_death_clears_obstructed_overworld_bed();
	test_dispenser_death_commits_and_replays();
	drain_run = 0;
	thread_join(&drain);
	const char* worlds[] = {
		WORLD_A, WORLD_B, WORLD_C, WORLD_D, WORLD_E,
		WORLD_F, WORLD_G, WORLD_CONTAINER_AMEND, WORLD_I, WORLD_J, WORLD_K,
		WORLD_L,
		WORLD_M,
		WORLD_N,
		WORLD_O,
		WORLD_DISPENSER_LIVE,
		WORLD_DISPENSER_REPLAY,
		WORLD_BATCH_LIVE,
	};
	for(size_t i = 0; i < sizeof(worlds) / sizeof(worlds[0]); i++)
		remove_world_dirs(worlds[i]);
	puts("test_death_drop_journal_env: OK");
	return 0;
}
