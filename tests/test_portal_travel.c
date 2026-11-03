/*
	Beta 1.7.3 nether-portal goldens, part 1: IGNITION + SELF-BREAK (decompile 740c583),
	over a real superflat server_world.

	Ignition (BlockFire.onBlockAdded head -> BlockPortal.tryToCreatePortal, dispatched from
	the server_world_set_block BLOCK_FIRE chokepoint):
	  - a corners-EXEMPT 4x5 obsidian frame ignites into an exact 2x3 portal interior
	    (both X- and Z-axis frames; the corner cells are deliberately absent to prove
	    the exemption);
	  - the air-conditional anchor shift (fire in either interior column -> same grid);
	  - rejections: missing border block, occupied interior, obsidian on BOTH axes;
	  - completing a frame around already-BURNING fire ignites on the next fire tick
	    (Beta setBlockIDWithMetadata re-runs onBlockAdded on age rewrites);
	  - the fill runs under world.editingBlocks: the very success of the frame fill
	    proves the suppression (an unsuppressed second interior set would notify the
	    first portal cell, whose column is then 2 tall != 3 -> it would self-break).

	Self-break (BlockPortal.onNeighborBlockChange):
	  - breaking a frame obsidian cascades the WHOLE pane to air;
	  - a lone unframed portal block breaks on any neighbour change (default-Z rule);
	  - a hand-built 4-tall portal column breaks (the exactly-3-tall rule).

	Everything here is deterministic -- no RNG draws on any asserted path.
*/

#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

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
#include "block/piston_moving.h"
#include "entity/entity_fish.h"
#include "entity/entity_primed_tnt.h"
#include "entity/combat.h" // PLAYER_EYE_HEIGHT
#include "game/game_state.h"
#include "game/gui/loading_tips.h"
#include "item/items.h"
#include "network/client_interface.h"
#include "network/complex_block_archive.h"
#include "network/entity_archive.h"
#include "network/inventory_logic.h"
#include "network/level_archive.h"
#include "network/portal_teleporter.h"
#include "network/region_archive.h"
#include "network/server_interface.h"
#include "network/server_furnace.h"
#include "network/server_local.h"
#include "network/server_local_state.h" // full server state (split from server_local.h)
#include "network/server_world.h"
#include "network/tile_entities_store.h"
#include "particle.h"
#include "platform/thread.h"

union sized_dict_allocation_header {
	size_t bytes;
	long double alignment;
	void* pointer_alignment;
};

static unsigned sized_dict_realloc_calls;
static unsigned sized_dict_realloc_fail_countdown;
static bool sized_dict_count_mismatch;

static void* sized_dict_realloc(void* ptr, size_t old_bytes,
							   size_t new_bytes) {
	sized_dict_realloc_calls++;
	if(sized_dict_realloc_fail_countdown > 0
	   && --sized_dict_realloc_fail_countdown == 0)
		return NULL;
	union sized_dict_allocation_header* old_header = NULL;
	if(ptr) {
		old_header = (union sized_dict_allocation_header*)ptr - 1;
		if(old_header->bytes != old_bytes)
			sized_dict_count_mismatch = true;
	}
	union sized_dict_allocation_header* fresh
		= malloc(sizeof(*fresh) + new_bytes);
	if(!fresh)
		return NULL;
	fresh->bytes = new_bytes;
	if(ptr)
		memcpy(fresh + 1, ptr,
			   old_bytes < new_bytes ? old_bytes : new_bytes);
	free(old_header);
	return fresh + 1;
}

static void sized_dict_free(void* ptr, size_t bytes) {
	if(!ptr)
		return;
	union sized_dict_allocation_header* header
		= (union sized_dict_allocation_header*)ptr - 1;
	if(header->bytes != bytes)
		sized_dict_count_mismatch = true;
	free(header);
}

#define SIZED_DICT_REALLOC(ctx, type, ptr, old_count, new_count)              \
	((type*)sized_dict_realloc(                                                \
		(ptr), (old_count) * sizeof(type), (new_count) * sizeof(type)))
#define SIZED_DICT_FREE(ctx, type, ptr, count)                                \
	(sized_dict_free((ptr), (count) * sizeof(type)))
#define SIZED_DICT_KEY_OPLIST                                                 \
	M_OPEXTEND(M_BASIC_OPLIST, REALLOC(SIZED_DICT_REALLOC),                    \
			   FREE(SIZED_DICT_FREE))
DICT_DEF2(dict_sized_probe, uint32_t, SIZED_DICT_KEY_OPLIST, uint32_t,
		  M_POD_OPLIST)
#undef SIZED_DICT_KEY_OPLIST
#undef SIZED_DICT_FREE
#undef SIZED_DICT_REALLOC

extern void server_local_spawn_block_drops_with_chance(
	struct server_world* w, struct block_info* blk_info, float chance);
extern void server_local_test_retry_orphaned_container_drops(
	struct server_local* s);

static char s_fixture_TMP_WORLD[128]; /* was: #define TMP_WORLD "test_portal_tmp" */
#define TMP_WORLD s_fixture_TMP_WORLD
/* test_portal_tmp2 is a SECOND, separate fixture world used by the
 * checkpoint/rename-vs-legacy-migration tests further down this file --
 * shared across several file-scope helper functions below, so it needs a
 * file-scope buffer of its own (populated in main(), same as TMP_WORLD). */
static char s_fixture_test_portal_tmp2[128];
#define TEST_PORTAL_TMP2 s_fixture_test_portal_tmp2
#define Y 70

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

/* Production runs the server at 20 TPS: between two ticks the client drains
 * its inbox, so deferred output (SOL-REVIEW-d42f, bf92f3e33: the server never
 * blocks on a full inbox, it queues and retries) never outlives a tick there.
 * This fixture steps ticks back-to-back with drain_entry as the client, so
 * the producer can outrun the consumer by thousands of ticks and trip the
 * 6000-tick LOAD_WATCHDOG while near chunks are still waiting on tokens. Give
 * the consumer the inter-tick window production gives it: wait (bounded)
 * until all deferred output is delivered and `tokens` inbox tokens are free.
 * This paces only the harness; every server-side gate is unchanged. */
static void wait_client_consumer(unsigned tokens) {
	for(int waited_ms = 0; !clin_rpc_headroom(tokens); waited_ms++) {
		assert(waited_ms < 5000 && "drain_entry never caught up");
		thread_msleep(1);
	}
}

struct dict_transaction_pause {
	struct thread_channel entered;
	struct thread_channel release;
};

static void pause_dictionary_transaction(void* arg) {
	struct dict_transaction_pause* pause = arg;
	void* token = pause;
	assert(tchannel_send(&pause->entered, token, true));
	assert(tchannel_receive(&pause->release, &token, true));
	assert(token == pause);
}

struct dict_contention_signal {
	struct thread_channel observed;
	unsigned sent;
};

static void signal_dictionary_contention(void* arg) {
	struct dict_contention_signal* signal = arg;
	if(__atomic_exchange_n(&signal->sent, 1, __ATOMIC_ACQ_REL) == 0)
		assert(tchannel_send(&signal->observed, signal, true));
}

struct entity_reserve_thread_args {
	struct dict_entity_s* dict;
	bool result;
};

static void* entity_reserve_thread(void* arg) {
	struct entity_reserve_thread_args* args = arg;
	args->result = dict_entity_try_reserve(args->dict, 1024);
	return NULL;
}

struct world_insert_thread_args {
	struct world* world;
	w_coord_t x;
	w_coord_t z;
	struct thread_channel* started;
	struct thread_channel* completed;
};

static void* world_insert_thread(void* arg) {
	struct world_insert_thread_args* args = arg;
	if(args->started)
		assert(tchannel_send(args->started, args, true));
	world_set_block(args->world, args->x, 70, args->z,
					(struct block_data) {.type = BLOCK_STONE}, false);
	if(args->completed)
		assert(tchannel_send(args->completed, args, true));
	return NULL;
}

static void set(int x, int y, int z, int type, int meta) {
	bool changed = server_world_set_block(
		&s.worlds[0], x, y, z,
		(struct block_data) {.type = (uint8_t)type,
						 .metadata = (uint8_t)(meta & 0x0F)});
	if(!changed) {
		/* Frame construction is intentionally reusable across cases. Accept only
		 * the exact live state; an unresident/mismatched failure still aborts. */
		struct block_data live;
		assert(server_world_get_block(&s.world, x, y, z, &live));
		assert(live.type == (uint8_t)type);
		assert(live.metadata == (uint8_t)(meta & 0x0F));
	}
}
static int gid(int x, int y, int z) {
	struct block_data b;
	return server_world_get_block(&s.world, x, y, z, &b) ? b.type : -1;
}
// Drive fire's scheduled tick directly (the age-rewrite path).
static void fire_tick(int x, int y, int z) {
	struct block_data fd;
	if(!server_world_get_block(&s.world, x, y, z, &fd) || fd.type != BLOCK_FIRE)
		return;
	struct block_info info = {.block = &fd, .neighbours = NULL, .x = x, .y = y, .z = z};
	blocks[BLOCK_FIRE]->onWorldTick(&s, &info);
}

// Build a corners-EXEMPT 4x5 obsidian frame whose 2x3 interior spans
// (ax..ax+dx, ay..ay+2, az..az+dz) with (dx,dz) = (1,0) for an X-axis portal
// or (0,1) for Z. skip_border: linear index [0..9] of one border block to
// LEAVE OUT (-1 = complete frame). Border cells: 2 bottom + 2 top + 3 per side.
static void build_frame(int ax, int ay, int az, int dx, int dz, int skip_border) {
	int k = 0;
	for(int i = -1; i <= 2; i++) {
		for(int j = -1; j <= 3; j++) {
			bool corner = (i == -1 || i == 2) && (j == -1 || j == 3);
			bool border = (i == -1 || i == 2 || j == -1 || j == 3);
			if(corner || !border)
				continue;
			if(k++ == skip_border)
				continue;
			set(ax + dx * i, ay + j, az + dz * i, BLOCK_OBSIDIAN, 0);
		}
	}
}
// Assert the exact post-ignition grid: 6 interior cells are portal, borders
// are still obsidian, and the corner cells are untouched (air).
static void assert_portal_grid(int ax, int ay, int az, int dx, int dz) {
	for(int i = -1; i <= 2; i++) {
		for(int j = -1; j <= 3; j++) {
			int cx = ax + dx * i, cy = ay + j, cz = az + dz * i;
			bool corner = (i == -1 || i == 2) && (j == -1 || j == 3);
			bool border = (i == -1 || i == 2 || j == -1 || j == 3);
			if(corner)
				assert(gid(cx, cy, cz) == BLOCK_AIR);
			else if(border)
				assert(gid(cx, cy, cz) == BLOCK_OBSIDIAN);
			else
				assert(gid(cx, cy, cz) == BLOCK_PORTAL);
		}
	}
}
static int count_portal(int x0, int y0, int z0, int x1, int y1, int z1) {
	int n = 0;
	for(int x = x0; x <= x1; x++)
		for(int y = y0; y <= y1; y++)
			for(int z = z0; z <= z1; z++)
				if(gid(x, y, z) == BLOCK_PORTAL)
					n++;
	return n;
}

static int finish_portal_travel(struct server_local* server) {
	/* At the loading-screen 2 ms pacing floor, 4096 slices are already more
	 * than eight seconds before any compute. A larger bound hid the hardware's
	 * 68-second exhaustive-scan regression instead of testing playability. */
	for(int i = 0; i < 4096; i++)
		if(server_local_portal_travel_step(server))
			return i + 1;
	assert(!"cooperative portal job did not finish within the test bound");
	return 0;
}

/* These were `static const char*[]` literals rooted at "test_portal_tmp2/..."
 * directly; now that the root is a runtime-unique path (TEST_PORTAL_TMP2),
 * the array elements can no longer be compile-time string constants, so the
 * arrays hold their own persistent buffers instead of pointers into the
 * (small, rotating) test_fixture_join() ring -- these are read back long
 * after other join() calls would have overwritten a borrowed slot. Filled
 * once by test_portal_tmp2_paths_init(), called from main() right after
 * TEST_PORTAL_TMP2 itself is established. */
static char destination_region_files[4][192];
static char source_region_files[2][192];

static void test_portal_tmp2_paths_init(void) {
	snprintf(destination_region_files[0], sizeof(destination_region_files[0]),
			 "%s/DIM-1/region/r.-1.-1.mcr", TEST_PORTAL_TMP2);
	snprintf(destination_region_files[1], sizeof(destination_region_files[1]),
			 "%s/DIM-1/region/r.-1.0.mcr", TEST_PORTAL_TMP2);
	snprintf(destination_region_files[2], sizeof(destination_region_files[2]),
			 "%s/DIM-1/region/r.0.-1.mcr", TEST_PORTAL_TMP2);
	snprintf(destination_region_files[3], sizeof(destination_region_files[3]),
			 "%s/DIM-1/region/r.0.0.mcr", TEST_PORTAL_TMP2);
	snprintf(source_region_files[0], sizeof(source_region_files[0]),
			 "%s/region/r.0.-1.mcr", TEST_PORTAL_TMP2);
	snprintf(source_region_files[1], sizeof(source_region_files[1]),
			 "%s/region/r.0.0.mcr", TEST_PORTAL_TMP2);
}

static void clear_destination_regions(void) {
	for(size_t i = 0; i < sizeof(destination_region_files)
								 / sizeof(destination_region_files[0]);
		i++)
		remove(destination_region_files[i]);
}

static void clear_destination_legacy(void) {
	static const char* suffixes[] = {
		"/chests.dat",
		"/chests.dat_old",
		"/chests.dat_new",
		"/signs.dat",
		"/signs.dat_old",
		"/signs.dat_new",
		"/chests_dim-1.dat",
		"/chests_dim-1.dat_old",
		"/chests_dim-1.dat_new",
		"/signs_dim-1.dat",
		"/signs_dim-1.dat_old",
		"/signs_dim-1.dat_new",
	};
	for(size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++)
		remove(test_fixture_join(TEST_PORTAL_TMP2, suffixes[i]));
}

static void clear_source_regions(void) {
	for(size_t i = 0;
		i < sizeof(source_region_files) / sizeof(source_region_files[0]); i++)
		remove(source_region_files[i]);
}

static bool destination_region_exists(void) {
	for(size_t i = 0; i < sizeof(destination_region_files)
								 / sizeof(destination_region_files[0]);
		i++) {
		FILE* file = fopen(destination_region_files[i], "rb");
		if(file) {
			fclose(file);
			return true;
		}
	}
	return false;
}

static void assert_saved_player_is_source(const char* world_name) {
	string_t level_name;
	string_init_set_str(level_name, world_name);
	struct level_archive level;
	assert(level_archive_create(&level, level_name));
	vec3 pos;
	enum world_dim dimension = WORLD_DIM_NETHER;
	assert(level_archive_read_player(&level, pos, NULL, NULL, &dimension));
	level_archive_destroy(&level);
	string_clear(level_name);
	assert(dimension == WORLD_DIM_OVERWORLD);
	assert(fabs(pos[0] - 100.0) < 1.0e-9);
	assert(fabs(pos[1] - 70.0) < 1.0e-9);
	assert(fabs(pos[2] - (-60.0)) < 1.0e-9);
}

static size_t count_saved_destination_portal_blocks(void) {
	string_t level_name;
	string_init_set_str(level_name, TEST_PORTAL_TMP2);
	static struct server_world disk_world;
	server_world_create(
		&disk_world, level_name, WORLD_DIM_NETHER, 4242, true);

	size_t portals = 0;
	/* The destination starts at (12.5,-7.5) and Teleporter's creation search is
	 * bounded to 16 blocks (plus frame clearance). This wider chunk window reads
	 * only indexed chunks, never generating an absent one. */
	for(w_coord_t cz = -3; cz <= 3; cz++)
		for(w_coord_t cx = -3; cx <= 3; cx++) {
			if(!server_world_disk_has_chunk(&disk_world, cx, cz))
				continue;
			struct server_chunk* chunk = NULL;
			assert(server_world_load_chunk(&disk_world, cx, cz, &chunk));
			assert(chunk && chunk->ids);
			for(size_t i = 0;
				i < (size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT; i++)
				if(chunk->ids[i] == BLOCK_PORTAL)
					portals++;
		}
	server_world_discard(&disk_world);
	string_clear(level_name);
	return portals;
}

static void assert_source_rollback(struct server_local* server,
								   int cooldown, float overlay) {
	assert(server->player.has_pos);
	assert(server->player.dimension == WORLD_DIM_OVERWORLD);
	assert(server->world.dimension == WORLD_DIM_OVERWORLD);
	assert(fabs(server->player.x - 100.0) < 1.0e-9);
	assert(fabs(server->player.y - 70.0) < 1.0e-9);
	assert(fabs(server->player.z - (-60.0)) < 1.0e-9);
	assert(server->player.fire == 123 && server->player.oxygen == 55);
	assert(fabsf(server->player.vel_x - 0.375f) < 1.0e-6f);
	assert(fabsf(server->player.vel_y - (-0.5f)) < 1.0e-6f);
	assert(fabsf(server->player.old_vel_y - (-0.25f)) < 1.0e-6f);
	assert(fabsf(server->player.vel_z - 0.625f) < 1.0e-6f);
	assert(fabsf(server->player.fall_distance - 13.25F) < 1.0e-6F);
	assert(server->player.on_ground && server->player.sneaking);
	assert(server->player.distance_walked == 3.75F);
	assert(server->player.next_step_distance == 6);
	assert(server->player.time_until_portal == cooldown);
	assert(fabsf(server->player.time_in_portal - overlay) < 1.0e-6f);
	assert(!server->portal_job && !server->portal_origin.valid);
	assert(server->portal_origin.rollback_retry_count == 0);
}

static void assert_inventory_items_equal(
	const struct inventory* actual,
	const struct item_data expected[INVENTORY_SIZE]) {
	assert(actual && actual->capacity == INVENTORY_SIZE);
	for(size_t i = 0; i < INVENTORY_SIZE; i++) {
		assert(actual->items[i].id == expected[i].id);
		assert(actual->items[i].durability == expected[i].durability);
		assert(actual->items[i].count == expected[i].count);
	}
}

static void finish_and_assert_portal_inventory(
	struct server_local* server,
	const struct item_data expected[INVENTORY_SIZE], size_t hotbar) {
	server_local_test_finish_load_inventory(server);
	server_local_test_world_switch_player_pos_queued(server);
	assert_inventory_items_equal(&server->player.inventory, expected);
	assert(inventory_get_hotbar(&server->player.inventory) == hotbar);
	assert(!inventory_get_picked_item(&server->player.inventory, NULL));
}

static int count_item_entities(struct server_local* server, int16_t id,
							   uint8_t count) {
	int matches = 0;
	dict_entity_it_t it;
	dict_entity_it(it, server->entities);
	while(!dict_entity_end_p(it)) {
		const struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && e->data.item.item.id == id
		   && e->data.item.item.count == count)
			matches++;
		dict_entity_next(it);
	}
	return matches;
}

static int count_item_entities_exact(struct server_local* server, int16_t id,
								 uint16_t durability, uint8_t count) {
	int matches = 0;
	dict_entity_it_t it;
	dict_entity_it(it, server->entities);
	while(!dict_entity_end_p(it)) {
		const struct entity* e = dict_entity_ref(it)->value;
		if(e && e->type == ENTITY_ITEM && e->data.item.item.id == id
		   && e->data.item.item.durability == durability
		   && e->data.item.item.count == count)
			matches++;
		dict_entity_next(it);
	}
	return matches;
}

static void load_live_entity_owner_chunks(struct server_local* server) {
	assert(server);
	for(size_t attempts = 0;; attempts++) {
		bool missing = false;
		w_coord_t missing_x = 0, missing_z = 0;
		dict_entity_it_t it;
		dict_entity_it(it, server->entities);
		while(!dict_entity_end_p(it)) {
			const struct entity* e = dict_entity_ref(it)->value;
			dict_entity_next(it);
			if(!e || e->type == ENTITY_LOCAL_PLAYER)
				continue;
			double cx = floor((double)e->pos[0] / CHUNK_SIZE);
			double cz = floor((double)e->pos[2] / CHUNK_SIZE);
			assert(isfinite(cx) && isfinite(cz));
			assert(cx >= INT32_MIN && cx <= INT32_MAX
				   && cz >= INT32_MIN && cz <= INT32_MAX);
			missing_x = (w_coord_t)cx;
			missing_z = (w_coord_t)cz;
			if(!server_world_is_chunk_loaded(
				   &server->world, missing_x, missing_z)) {
				missing = true;
				break;
			}
		}
		if(!missing)
			return;
		assert(attempts < 1024); /* malformed archive cannot grow forever */
		struct server_chunk* owner = NULL;
		assert(server_world_load_chunk(
			&server->world, missing_x, missing_z, &owner));
		assert(owner);
	}
}

struct world_switch_rpc_capture {
	enum client_rpc_type events[8];
	size_t count;
	size_t chunk_count;
	size_t player_pos_count;
	size_t load_failed_count;
	uint32_t input_epoch;
	bool has_input_handoff;
	struct client_rpc input_handoff;
	size_t handoff_count;
	enum server_world_switch_phase handoff_phase;
};

static void capture_world_switch_rpc(const struct client_rpc* call, void* user) {
	struct world_switch_rpc_capture* capture = user;
	if(call->type == CRPC_CHUNK)
		capture->chunk_count++;
	else if(call->type == CRPC_LOAD_FAILED)
		capture->load_failed_count++;
	else if(call->type == CRPC_PLAYER_POS) {
		capture->player_pos_count++;
		if(call->payload.player_pos.input_epoch != 0) {
			capture->input_epoch = call->payload.player_pos.input_epoch;
			capture->has_input_handoff = true;
			capture->input_handoff = *call;
		}
	}
	if(call->type != CRPC_RETURN_INGAME
	   && call->type != CRPC_BEGIN_WORLD_SWITCH
	   && call->type != CRPC_WORLD_RESET)
		return;
	assert(capture->count < sizeof(capture->events) / sizeof(capture->events[0]));
	capture->events[capture->count++] = call->type;
}

static void test_initial_load_scratch_oom_reports_terminal(void) {
	/* The session reservation is attempted eagerly and once more immediately
	 * before an ordinary world load. If both allocations fail, has_pos never
	 * becomes true, so the streaming watchdog cannot own this failure. The RPC
	 * boundary must publish one terminal result instead of hanging forever. */
	server_local_test_fail_populate_scratch_allocs(2);
	static struct server_local server;
	server_local_test_create_without_thread(
		&server, false, false, false, false, false, true, false);
	assert(!server.world_populate_scratch && !server.player.has_pos);

	struct world_switch_rpc_capture capture = {0};
	clin_test_set_rpc_observer(capture_world_switch_rpc, &capture);
	struct server_rpc load = {.type = SRPC_LOAD_WORLD};
	string_init_set_str(load.payload.load_world.name, "scratch_oom_world");
	server_local_test_process_rpc(&server, &load);
	assert(server.load_failure_terminal && !server.player.has_pos);
	assert(!server.world_populate_scratch);
	assert(capture.load_failed_count == 1);

	/* Terminal state is one-shot until process restart, matching the visible
	 * recovery instruction; a delayed duplicate LOAD cannot spam the client. */
	struct server_rpc duplicate = {.type = SRPC_LOAD_WORLD};
	string_init_set_str(duplicate.payload.load_world.name, "scratch_oom_world");
	server_local_test_process_rpc(&server, &duplicate);
	assert(capture.load_failed_count == 1);
	clin_test_set_rpc_observer(NULL, NULL);
	clin_drain_discard();
	puts("  portal: initial populate-scratch OOM reports terminal LOAD_FAILED");
}

static int finish_real_world_load(
	struct server_local* server, struct world_switch_rpc_capture* capture) {
	assert(server && capture && server->player.has_pos
		   && !server->player.finished_loading);
	size_t player_pos_before = capture->player_pos_count;
	uint64_t world_time_before = server->world_time;
	short health_before = server->player.health;
	int fire_before = server->player.fire;
	int oxygen_before = server->player.oxygen;
	int hurt_resist_before = server->player.hurt_resist_time;
	int portal_cooldown_before = server->player.time_until_portal;
	float portal_overlay_before = server->player.time_in_portal;
	for(int tick = 1; tick <= 10000; tick++) {
		/* The test-only constructor deliberately has no racing background thread.
		 * Lift the fixture's manual-step pause only for this exact production update
		 * slice, then restore it for the explicit portal transaction steps. */
		bool was_paused = server->paused;
		server->paused = false;
		server_local_test_update_once(server);
		server->paused = was_paused;
		wait_client_consumer(1);
		if(server->player.finished_loading
		   && capture->player_pos_count > player_pos_before) {
			/* Loading is generation/streaming only. A slow card must not advance
			 * simulation or mutate the controller-less player behind the screen. */
			assert(server->world_time == world_time_before);
			assert(server->player.health == health_before);
			assert(server->player.fire == fire_before);
			assert(server->player.oxygen == oxygen_before);
			assert(server->player.hurt_resist_time == hurt_resist_before);
			assert(server->player.time_until_portal
				   == portal_cooldown_before);
			assert(server->player.time_in_portal == portal_overlay_before);
			assert(capture->input_epoch != 0);
			assert(capture->has_input_handoff);
			const struct client_rpc* handoff = &capture->input_handoff;
			assert(handoff->payload.player_pos.selected_hotbar
				   == inventory_get_hotbar(&server->player.inventory));
			assert(fabsf(handoff->payload.player_pos.velocity[0]
						 - server->player.vel_x) < 1.0e-6f);
			assert(fabsf(handoff->payload.player_pos.velocity[1]
						 - server->player.vel_y) < 1.0e-6f);
			assert(fabsf(handoff->payload.player_pos.velocity[2]
						 - server->player.vel_z) < 1.0e-6f);
			assert(handoff->payload.player_pos.oxygen == server->player.oxygen);
			assert(handoff->payload.player_pos.on_fire
				   == (server->player.fire > 0));
			assert(handoff->payload.player_pos.on_ground
				   == server->player.on_ground);
			assert(handoff->payload.player_pos.sneaking
				   == server->player.sneaking);
			assert(handoff->payload.player_pos.world_time == server->world_time);
			assert(handoff->payload.player_pos.fresh_body
				   == (server->world_switch_phase == SERVER_WORLD_SWITCH_DEATH));
			assert(server->input_ack_pending == capture->input_epoch);
			assert(server->world_switch_phase != SERVER_WORLD_SWITCH_IDLE);
			/* A pre-BEGIN message surviving in the opposite-direction FIFO must
			 * remain rejected even after destination PLAYER_POS was queued. */
			bool paused_before_ack = server->paused;
			struct server_rpc stale_toggle = {.type = SRPC_TOGGLE_PAUSE};
			server_local_test_process_rpc(server, &stale_toggle);
			assert(server->paused == paused_before_ack);
			struct server_rpc ack = {
				.type = SRPC_INPUT_READY,
				.payload.input_ready.epoch = capture->input_epoch,
			};
			server_local_test_process_rpc(server, &ack);
			assert(server->input_ack_pending == 0);
			assert(server->world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
			assert(capture->player_pos_count == player_pos_before + 1);
			w_coord_t pcx = WCOORD_CHUNK_OFFSET(floor(server->player.x));
			w_coord_t pcz = WCOORD_CHUNK_OFFSET(floor(server->player.z));
			for(w_coord_t z = pcz - MAX_VIEW_DISTANCE;
				z <= pcz + MAX_VIEW_DISTANCE; z++)
				for(w_coord_t x = pcx - MAX_VIEW_DISTANCE;
					x <= pcx + MAX_VIEW_DISTANCE; x++)
					assert(server_world_is_chunk_sent(&server->world, x, z));
			return tick;
		}
	}
	assert(!"real near-chunk load did not reach CRPC_PLAYER_POS within bound");
	return 0;
}

static void assert_world_switch_rpcs(
	const struct world_switch_rpc_capture* capture,
	const enum client_rpc_type* expected, size_t count) {
	assert(capture->count == count);
	for(size_t i = 0; i < count; i++)
		assert(capture->events[i] == expected[i]);
}

static void acknowledge_input_direct(struct server_local* server) {
	assert(server && server->input_ack_pending != 0);
	uint32_t epoch = server->input_ack_pending;
	struct server_rpc ack = {
		.type = SRPC_INPUT_READY,
		.payload.input_ready.epoch = epoch,
	};
	server_local_test_process_rpc(server, &ack);
	assert(server->input_ack_pending == 0);
	assert(server->world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
}

static void capture_world_switch_handoff(
	enum server_world_switch_phase phase, void* user) {
	struct world_switch_rpc_capture* capture = user;
	// This callback runs after clin_rpc_send returned and thread_yield was
	// invoked. BEGIN must therefore already be FIFO-enqueued for the main thread.
	assert(capture->count > 0);
	assert(capture->events[capture->count - 1] == CRPC_BEGIN_WORLD_SWITCH);
	capture->handoff_count++;
	capture->handoff_phase = phase;
}

static void assert_world_switch_barrier_blocks(
	struct server_local* server, enum server_world_switch_phase phase) {
	assert(server->world_switch_phase == phase);
	const bool paused = server->paused;
	const bool has_pos = server->player.has_pos;
	const bool finished_loading = server->player.finished_loading;
	const double x = server->player.x, y = server->player.y, z = server->player.z;
	const enum world_dim player_dim = server->player.dimension;
	const enum world_dim world_dim = server->world.dimension;
	struct portal_teleporter_job* job = server->portal_job;
	const bool origin_valid = server->portal_origin.valid;
	string_t level_name;
	string_init_set(level_name, server->level_name);

	struct server_rpc toggle = {.type = SRPC_TOGGLE_PAUSE};
	server_local_test_process_rpc(server, &toggle);
	struct server_rpc unload = {.type = SRPC_UNLOAD_WORLD};
	server_local_test_process_rpc(server, &unload);
	struct server_rpc pos = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {.x = x + 1000.0, .y = y + 100.0, .z = z - 1000.0},
	};
	server_local_test_process_rpc(server, &pos);
	struct server_rpc load = {.type = SRPC_LOAD_WORLD};
	string_init_set_str(load.payload.load_world.name, "stale_transition_load");
	server_local_test_process_rpc(server, &load); // owns + clears the string

	assert(server->world_switch_phase == phase);
	assert(server->paused == paused);
	assert(server->player.has_pos == has_pos);
	assert(server->player.finished_loading == finished_loading);
	assert(server->player.x == x && server->player.y == y && server->player.z == z);
	assert(server->player.dimension == player_dim);
	assert(server->world.dimension == world_dim);
	assert(server->portal_job == job);
	assert(server->portal_origin.valid == origin_valid);
	assert(strcmp(string_get_cstr(server->level_name),
				  string_get_cstr(level_name)) == 0);
	string_clear(level_name);
}

struct close_transport_capture {
	uint16_t action_ids[64];
	size_t action_count;
	int close_count;
	bool action_after_close;
};

static void capture_close_transport(struct server_rpc* call, void* user) {
	struct close_transport_capture* capture = user;
	if(call->type == SRPC_WINDOW_CLICK) {
		assert(capture->action_count < 64);
		if(capture->close_count > 0)
			capture->action_after_close = true;
		capture->action_ids[capture->action_count++]
			= call->payload.window_click.action_id;
	} else if(call->type == SRPC_WINDOW_CLOSE) {
		capture->close_count++;
	}
}

static void test_reserved_close_transport(void) {
	assert(svin_pending_count() == 0);
	struct window_container wc;
	assert(windowc_create(&wc, WINDOW_TYPE_INVENTORY, INVENTORY_SIZE));
	/* Exercise a normal bag slot: crafting output 0 now obeys prediction
	 * rules and cannot be used as an arbitrary pick/place transport fixture. */
	windowc_slot_change(
		&wc, INVENTORY_SLOT_MAIN, (struct item_data) {.id = ITEM_APPLE, .count = 1});

	// Hold all 16 live tokens, then create 64 genuine optimistic revisions in
	// the ordinary FIFO. The lifecycle CLOSE must occupy its reserved 65th
	// position without evicting any revision-bearing click.
	for(int i = 0; i < 16; i++) {
		struct server_rpc pos = {.type = SRPC_PLAYER_POS};
		assert(svin_rpc_send(&pos));
	}
	uint16_t expected_ids[64];
	for(int i = 0; i < 64; i++) {
		assert(windowc_new_action(&wc, &expected_ids[i], false, INVENTORY_SLOT_MAIN));
		struct server_rpc click = {
			.type = SRPC_WINDOW_CLICK,
			.payload.window_click = {
				.window = WINDOWC_INVENTORY,
				.slot = INVENTORY_SLOT_MAIN,
				.action_id = expected_ids[i],
			},
		};
		assert(svin_rpc_send(&click));
	}
	assert(svin_pending_count() == 64);

	// A 65th optimistic click cannot enter the ordinary budget. The real GUI
	// call sites use this exact false result to reject their newest revision.
	struct item_data slot_before, picked_before;
	assert(inventory_get_slot(windowc_get_latest(&wc), INVENTORY_SLOT_MAIN, &slot_before));
	bool had_picked_before
		= inventory_get_picked_item(windowc_get_latest(&wc), &picked_before);
	uint16_t rejected_id;
	assert(windowc_new_action(&wc, &rejected_id, false, INVENTORY_SLOT_MAIN));
	struct server_rpc rejected = {
		.type = SRPC_WINDOW_CLICK,
		.payload.window_click = {
			.window = WINDOWC_INVENTORY,
			.slot = INVENTORY_SLOT_MAIN,
			.action_id = rejected_id,
		},
	};
	assert(!svin_rpc_send(&rejected));
	windowc_action_apply_result(&wc, rejected_id, false);
	struct item_data slot_after, picked_after;
	assert(inventory_get_slot(windowc_get_latest(&wc), INVENTORY_SLOT_MAIN, &slot_after));
	assert(slot_before.id == slot_after.id
		   && slot_before.durability == slot_after.durability
		   && slot_before.count == slot_after.count);
	bool has_picked_after
		= inventory_get_picked_item(windowc_get_latest(&wc), &picked_after);
	assert(has_picked_after == had_picked_before);
	if(has_picked_after)
		assert(picked_before.id == picked_after.id
			   && picked_before.durability == picked_after.durability
			   && picked_before.count == picked_after.count);

	struct server_rpc close = {
		.type = SRPC_WINDOW_CLOSE,
		.payload.window_close.window = WINDOWC_INVENTORY,
	};
	assert(svin_rpc_send(&close));
	assert(svin_pending_count() == 65);

	struct close_transport_capture capture = {0};
	svin_process_messages(capture_close_transport, &capture, false);
	while(svin_pending_count() > 0) {
		svin_rpc_flush_pending();
		svin_process_messages(capture_close_transport, &capture, false);
	}
	assert(capture.action_count == 64 && capture.close_count == 1);
	assert(!capture.action_after_close);
	for(int i = 0; i < 64; i++)
		assert(capture.action_ids[i] == expected_ids[i]);
	windowc_destroy(&wc);
	puts("  portal: lifecycle reserve preserves every optimistic click before CLOSE");
}

struct pause_transport_capture {
	struct server_local* server;
	size_t dig_count;
	size_t toggle_count;
	size_t unload_count;
};

static void capture_pause_transport(struct server_rpc* call, void* user) {
	struct pause_transport_capture* capture = user;
	if(call->type == SRPC_BLOCK_DIG)
		capture->dig_count++;
	else if(call->type == SRPC_TOGGLE_PAUSE) {
		capture->toggle_count++;
		server_local_test_process_rpc(capture->server, call);
	} else if(call->type == SRPC_UNLOAD_WORLD)
		capture->unload_count++;
}

static void test_pause_toggle_transport(void) {
	assert(svin_pending_count() == 0);
	clin_drain_discard();
	/* Hold every live inbox token, then let a same-frame action consume the last
	 * ordinary pending slot. HOME must remain wholly local-no-op when its TOGGLE
	 * cannot enter the FIFO. */
	for(int i = 0; i < 16; i++) {
		struct server_rpc pos = {.type = SRPC_PLAYER_POS};
		assert(svin_rpc_send(&pos));
	}
	for(int i = 0; i < 63; i++) {
		struct server_rpc dig = {
			.type = SRPC_BLOCK_DIG,
			.payload.block_dig = {.x = i, .y = 64, .z = 0},
		};
		assert(svin_rpc_send(&dig));
	}
	struct server_rpc last_action = {
		.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {.x = 63, .y = 64, .z = 0},
	};
	assert(svin_rpc_send(&last_action));
	assert(svin_pending_count() == 64);

	memset(&gstate, 0, sizeof(gstate));
	gstate.players[0].current_screen = &screen_ingame;
	uint32_t drops_before = svin_dropped_count();
	assert(!screen_ingame_test_try_pause());
	assert(!gstate.paused && gstate.players[0].current_screen == &screen_ingame);
	assert(svin_pending_count() == 64);
	assert(svin_dropped_count() == drops_before + 1);

	struct server_local server = {0};
	server.player.has_pos = true;
	server.player.finished_loading = true;
	/* Deviation-#18: zero-init health reads as DEAD; be explicitly alive. */
	server.player.health = MAX_PLAYER_HEALTH;
	struct pause_transport_capture capture = {.server = &server};
	svin_process_messages(capture_pause_transport, &capture, false);
	while(svin_pending_count() > 0) {
		svin_rpc_flush_pending();
		svin_process_messages(capture_pause_transport, &capture, false);
	}
	assert(capture.dig_count == 64 && capture.toggle_count == 0);
	assert(!server.paused && !gstate.paused);

	/* Once capacity returns, one admitted pause and one admitted resume toggle
	 * leave both halves in the same state; the rejected attempt contributed none. */
	assert(screen_ingame_test_try_pause());
	assert(gstate.paused && gstate.players[0].current_screen == &screen_pause);
	svin_process_messages(capture_pause_transport, &capture, false);
	clin_drain_discard();
	assert(server.paused && capture.toggle_count == 1);
	struct server_rpc resume = {.type = SRPC_TOGGLE_PAUSE};
	assert(svin_rpc_send(&resume));
	gstate.paused = false;
	screen_set(&screen_ingame);
	svin_process_messages(capture_pause_transport, &capture, false);
	clin_drain_discard();
	assert(!server.paused && !gstate.paused);
	assert(capture.toggle_count == 2);

	/* A controller report may contain HOME+INVENTORY together. Once HOME's
	 * resume enters the FIFO it exclusively owns the frame: exactly one toggle,
	 * no save/unload, and both halves resume. */
	assert(screen_ingame_test_try_pause());
	svin_process_messages(capture_pause_transport, &capture, false);
	clin_drain_discard();
	assert(server.paused && gstate.paused);
	size_t toggles_before_edges = capture.toggle_count;
	size_t unloads_before_edges = capture.unload_count;
	screen_pause_test_process_edges(true, true);
	assert(!gstate.paused && gstate.players[0].current_screen == &screen_ingame);
	svin_process_messages(capture_pause_transport, &capture, false);
	clin_drain_discard();
	assert(!server.paused);
	assert(capture.toggle_count == toggles_before_edges + 1);
	assert(capture.unload_count == unloads_before_edges);
	puts("  portal: saturated pause + simultaneous HOME/INVENTORY stay synchronized");
}

static void* fail_client_alloc(size_t size);

struct input_gate_capture {
	size_t count;
	size_t ack_index;
	uint32_t ack_epoch;
	bool retry;
	size_t player_state_count;
	bool player_on_ground;
	bool player_sneaking;
	uint32_t player_correction_seq;
	size_t recover_count;
	uint32_t recover_epoch;
	size_t close_count;
	uint8_t close_window;
};

static void capture_input_gate_transport(struct server_rpc* call, void* user) {
	struct input_gate_capture* capture = user;
	if(call->type == SRPC_INPUT_READY) {
		capture->ack_index = capture->count;
		capture->ack_epoch = call->payload.input_ready.epoch;
		capture->retry = call->payload.input_ready.retry;
	} else if(call->type == SRPC_PLAYER_STATE) {
		capture->player_state_count++;
		capture->player_on_ground = call->payload.player_state.on_ground;
		capture->player_sneaking = call->payload.player_state.sneaking;
		capture->player_correction_seq
			= call->payload.player_state.correction_seq;
	} else if(call->type == SRPC_STREAM_RECOVER) {
		capture->recover_count++;
		capture->recover_epoch = call->payload.stream_recover.epoch;
	} else if(call->type == SRPC_WINDOW_CLOSE) {
		capture->close_count++;
		capture->close_window = call->payload.window_close.window;
	}
	capture->count++;
}

static struct client_rpc client_full_chunk_rpc(w_coord_t x, w_coord_t z) {
	struct chunk_rpc_slot* slot = clin_chunk_slot_acquire();
	memset(slot, 0, sizeof(*slot));
	return (struct client_rpc) {
		.type = CRPC_CHUNK,
		.payload.chunk = {
			.x = x,
			.y = 0,
			.z = z,
			.sx = CHUNK_SIZE,
			.sy = WORLD_HEIGHT,
			.sz = CHUNK_SIZE,
			.slot = slot,
		},
	};
}

static struct client_rpc client_far_chunk_rpc(w_coord_t x, w_coord_t z) {
	struct far_rpc_slot* slot = clin_far_slot_try_acquire();
	assert(slot);
	memset(slot, 0, sizeof(*slot));
	memset(slot->skin_y, 70, sizeof(slot->skin_y));
	memset(slot->top_id, BLOCK_STONE, sizeof(slot->top_id));
	/* Span more than one vertical chunk so failing allocation #2 proves that a
	 * partially allocated replacement is unwound without releasing the old one. */
	slot->skin_y[0] = 15;
	return (struct client_rpc) {
		.type = CRPC_FAR_CHUNK,
		.payload.far_chunk = {
			.x = x,
			.z = z,
			.slot = slot,
		},
	};
}

static void assert_far_slot_pool_fully_available(void) {
	/* busy_count is a monotonic exhaustion diagnostic, not a checked-out count.
	 * Prove conservation directly: after the client consumed an RPC, every pool
	 * owner must be acquirable, then return them without overflowing. */
	size_t capacity = clin_far_slot_test_capacity();
	assert(capacity > 0);
	struct far_rpc_slot* held[capacity];
	for(size_t k = 0; k < capacity; k++) {
		held[k] = clin_far_slot_try_acquire();
		assert(held[k] && "a FAR install/discard leaked its pooled payload");
		for(size_t prior = 0; prior < k; prior++)
			assert(held[k] != held[prior]
				   && "a FAR payload was returned to the pool more than once");
	}
	for(size_t k = 0; k < capacity; k++)
		clin_far_slot_release(held[k]);
}

struct stream_server_capture {
	struct server_local* server;
	enum client_rpc_type events[16];
	size_t count;
	bool claims_invalid_at_reset;
};

static void capture_stream_server_rpc(const struct client_rpc* call, void* user) {
	struct stream_server_capture* capture = user;
	assert(capture->count < sizeof(capture->events) / sizeof(capture->events[0]));
	capture->events[capture->count++] = call->type;
	if(call->type != CRPC_STREAM_RESET)
		return;
	bool invalid = !capture->server->player.finished_loading
		&& capture->server->pending_near_unload_count == 0
		&& dict_far_sent_size(capture->server->world.far_sent) == 0;
	dict_server_chunks_it_t it;
	dict_server_chunks_it(it, capture->server->world.chunks);
	while(!dict_server_chunks_end_p(it)) {
		invalid = invalid && !dict_server_chunks_ref(it)->value.sent;
		dict_server_chunks_next(it);
	}
	capture->claims_invalid_at_reset = invalid;
}

static void test_live_stream_recovery_server(void) {
	assert(svin_pending_count() == 0);
	clin_drain_discard();
	struct server_local server;
	memset(&server, 0, sizeof(server));
	server_local_test_create_without_thread(
		&server, false, false, false, false, false, true, false);
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	assert(server_world_create_with_populate_scratch(
		&server.world, level, WORLD_DIM_OVERWORLD, 0x173, true,
		server.world_populate_scratch));
	server.world.local = &server;
	server.worlds[0].session = &server; /* A1a (D1): owning session for services/callbacks */
	server.player.has_pos = true;
	server.player.finished_loading = true;
	server.player.x = 8.5;
	server.player.y = 70.0;
	server.player.z = 8.5;
	server.player.health = MAX_PLAYER_HEALTH;
	server.player.oxygen = 123;
	server.player.fire = 17;
	server.player.distance_walked = 7.25f;
	server.player.next_step_distance = 9;
	server.player.portal_travel_requested = true;
	server.input_epoch_counter = 500;
	server.paused = true;
	server.world_time = 12345;

	/* Make the collision center and its four covering populate quadrants
	 * resident/ready so the accepted request can send RESET then at least one
	 * replacement CHUNK in this exact update. */
	const w_coord_t ready[][2] = {{0, 0}, {-1, 0}, {0, -1}, {-1, -1}};
	for(size_t i = 0; i < sizeof(ready) / sizeof(ready[0]); i++) {
		struct server_chunk* sc = NULL;
		assert(server_world_load_chunk(
			&server.world, ready[i][0], ready[i][1], &sc));
		assert(sc);
		sc->populated = true;
		sc->sent = true;
	}
	struct inventory* retained_window = &server.player.inventory;
	server.player.active_inventory = retained_window;
	server_world_mark_far_sent(&server.world, 12, 12);
	server.pending_near_unload_count = 1;
	struct stream_server_capture capture = {.server = &server};
	clin_test_set_rpc_observer(capture_stream_server_rpc, &capture);

	struct server_rpc stale = {
		.type = SRPC_STREAM_RECOVER,
		.payload.stream_recover.epoch = 499,
	};
	server_local_test_process_rpc(&server, &stale);
	assert(server.player.finished_loading && capture.count == 0);
	assert(server_world_is_chunk_sent(&server.world, 0, 0));

	struct server_rpc exact = stale;
	exact.payload.stream_recover.epoch = 500;
	assert(svin_rpc_send(&exact));
	uint64_t world_time = server.world_time;
	short health = server.player.health;
	int oxygen = server.player.oxygen;
	int fire = server.player.fire;
	server_local_test_update_once(&server);
	assert(!server.player.finished_loading);
	assert(server.stream_recovery_epoch == 500);
	assert(server.player.active_inventory == retained_window);
	assert(server.paused && server.world_time == world_time);
	assert(server.player.health == health && server.player.oxygen == oxygen
		   && server.player.fire == fire);
	assert(server.player.distance_walked == 7.25f
		   && server.player.next_step_distance == 9);
	assert(server.player.portal_travel_requested);
	/* V6 F2a: the recovery is serviced in the network phase AFTER this
	 * update's world/streaming pass (Beta MinecraftServer.java:328-341:
	 * worlds tick, THEN the network, THEN trackers), so its near-column
	 * resend lands in the NEXT update's streaming pass. */
	server_local_test_update_once(&server);
	assert(capture.count >= 2 && capture.events[0] == CRPC_STREAM_RESET);
	assert(capture.claims_invalid_at_reset);
	bool saw_chunk = false;
	for(size_t i = 1; i < capture.count; i++)
		saw_chunk = saw_chunk || capture.events[i] == CRPC_CHUNK;
	assert(saw_chunk);

	/* Same/stale requests are idempotent once the exact one has closed the
	 * stable gate; neither may enqueue a second RESET. */
	size_t events_after_accept = capture.count;
	server_local_test_process_rpc(&server, &exact);
	server_local_test_process_rpc(&server, &stale);
	assert(capture.count == events_after_accept);
	clin_test_set_rpc_observer(NULL, NULL);
	/* A paused OPEN rejected by the client is FIFO-after STREAM_RECOVER. Its
	 * exact CLOSE must be admitted through the recovery gate, otherwise this
	 * authoritative container would remain hidden forever. */
	inventory_set_slot(
		&server.player.inventory, INVENTORY_SLOT_OUTPUT,
		(struct item_data) {.id = BLOCK_STONE, .count = 1});
	struct server_rpc recovery_close = {
		.type = SRPC_WINDOW_CLOSE,
		.payload.window_close.window = WINDOWC_INVENTORY,
	};
	server_local_test_process_rpc(&server, &recovery_close);
	assert(server.player.active_inventory == &server.player.inventory);
	assert(!inventory_get_slot(
		&server.player.inventory, INVENTORY_SLOT_OUTPUT, NULL));
	clin_drain_discard();
	server_world_discard(&server.world);
	server_world_populate_scratch_destroy(server.world_populate_scratch);
	server.world_populate_scratch = NULL;
	inventory_destroy(&server.player.inventory, &server.player);
	dict_entity_clear(server.entities);
	string_clear(server.level_name);
	string_clear(level);
	puts("  portal: exact live recovery gates simulation and resets claims FIFO-first");
}

static void test_dictionary_transaction_thread_ownership(void) {
	struct dict_transaction_pause pause;
	assert(tchannel_init(&pause.entered, 1));
	assert(tchannel_init(&pause.release, 1));
	void* token = NULL;

	/* entity.h is generated per translation unit, but two dictionaries in this
	 * TU still need exact thread ownership. Hold A after admission: B's checked
	 * transaction must fail before mutation, while an unrelated ordinary clear
	 * must not be mistaken for A's rollback journal. */
	dict_entity_t entity_a, entity_b, entity_foreign;
	dict_entity_init(entity_a);
	dict_entity_init(entity_b);
	dict_entity_init(entity_foreign);
	struct entity marker = {0};
	*dict_entity_safe_get(entity_a, 1) = &marker;
	*dict_entity_safe_get(entity_b, 2) = &marker;
	*dict_entity_safe_get(entity_foreign, 3) = &marker;
	dict_entity_test_set_transaction_hook(
		pause_dictionary_transaction, &pause);
	struct entity_reserve_thread_args entity_args = {
		.dict = entity_a,
	};
	struct thread entity_thread;
	assert(thread_create(
		&entity_thread, entity_reserve_thread, &entity_args, 32));
	assert(tchannel_receive(&pause.entered, &token, true));
	assert(token == &pause);
	struct dict_entity_s entity_b_before = *entity_b;
	assert(!dict_entity_try_reserve(entity_b, 1024));
	assert(entity_b->data == entity_b_before.data
		   && entity_b->index == entity_b_before.index
		   && entity_b->mask == entity_b_before.mask
		   && *dict_entity_get(entity_b, 2) == &marker);
	dict_entity_clear(entity_foreign);
	assert(tchannel_send(&pause.release, &pause, true));
	thread_join(&entity_thread);
	dict_entity_test_set_transaction_hook(NULL, NULL);
	assert(entity_args.result);
	assert(*dict_entity_get(entity_a, 1) == &marker);
	assert(dict_entity_try_reserve(entity_b, 1024));
	assert(*dict_entity_get(entity_b, 2) == &marker);
	dict_entity_clear(entity_a);
	dict_entity_clear(entity_b);

	/* world.c's allocator journal is shared by every world dictionary. The held
	 * admission schedule proves a distinct thread waits without reporting OOM,
	 * and a foreign dictionary free remains an ordinary free rather than being
	 * appended to A's journal or longjmp target. */
	struct world world_a, world_b;
	world_create(&world_a);
	world_create(&world_b);
	dict_wsection_t world_foreign;
	dict_wsection_init(world_foreign);
	memset(dict_wsection_safe_get(world_foreign, 4), 0,
		   sizeof(struct world_section));
	world_test_set_section_dict_transaction_hook(
		pause_dictionary_transaction, &pause);
	struct dict_contention_signal contention = {0};
	assert(tchannel_init(&contention.observed, 1));
	world_test_set_section_dict_contention_hook(
		signal_dictionary_contention, &contention);
	struct world_insert_thread_args world_args = {
		.world = &world_a,
		.x = 640,
		.z = 640,
	};
	struct thread world_thread_a;
	assert(thread_create(
		&world_thread_a, world_insert_thread, &world_args, 32));
	assert(tchannel_receive(&pause.entered, &token, true));
	assert(token == &pause);
	struct thread_channel world_started, world_completed;
	assert(tchannel_init(&world_started, 1));
	assert(tchannel_init(&world_completed, 1));
	struct world_insert_thread_args world_b_args = {
		.world = &world_b,
		.x = 656,
		.z = 656,
		.started = &world_started,
		.completed = &world_completed,
	};
	struct thread world_thread_b;
	assert(thread_create(
		&world_thread_b, world_insert_thread, &world_b_args, 32));
	assert(tchannel_receive(&world_started, &token, true));
	assert(token == &world_b_args);
	assert(tchannel_receive(&contention.observed, &token, true));
	assert(token == &contention);
	assert(!tchannel_receive(&world_completed, &token, false));
	dict_wsection_clear(world_foreign);
	/* B must acquire after A, but only A is deliberately paused. */
	world_test_set_section_dict_transaction_hook(NULL, NULL);
	assert(tchannel_send(&pause.release, &pause, true));
	thread_join(&world_thread_a);
	thread_join(&world_thread_b);
	assert(tchannel_receive(&world_completed, &token, true));
	assert(token == &world_b_args);
	world_test_set_section_dict_contention_hook(NULL, NULL);
	assert(world_find_chunk(&world_a, 640, 70, 640));
	assert(world_find_chunk(&world_b, 656, 70, 656));
	world_destroy(&world_a);
	world_destroy(&world_b);
	tchannel_close(&contention.observed);
	tchannel_close(&world_completed);
	tchannel_close(&world_started);

	tchannel_close(&pause.release);
	tchannel_close(&pause.entered);
	puts("  portal: dictionary OOM journals isolate foreign-thread ownership");
}

static void test_input_ack_transport_gate(void) {
	assert(svin_pending_count() == 0);
	clin_drain_discard();
	memset(&gstate, 0, sizeof(gstate));
	world_create(&gstate.world);
	dict_entity_init(gstate.entities);
	/* A growing M-lib dict allocates data, index, then bounded collision scratch.
	 * Failure of that third allocation must restore both primary buffers and every
	 * live value; the successful retry must also release scratch exactly once. */
	dict_entity_t entity_probe;
	dict_entity_init(entity_probe);
	struct entity entity_probe_a = {0}, entity_probe_b = {0};
	*dict_entity_safe_get(entity_probe, 11) = &entity_probe_a;
	*dict_entity_safe_get(entity_probe, 22) = &entity_probe_b;
	struct dict_entity_s entity_before = *entity_probe;
	dict_entity_test_fail_realloc_after(3);
	assert(!dict_entity_try_reserve(entity_probe, 1024));
	assert(entity_probe->data == entity_before.data
		   && entity_probe->index == entity_before.index
		   && entity_probe->mask == entity_before.mask
		   && entity_probe->count == entity_before.count
		   && entity_probe->count_delete == entity_before.count_delete);
	assert(*dict_entity_get(entity_probe, 11) == &entity_probe_a);
	assert(*dict_entity_get(entity_probe, 22) == &entity_probe_b);
	assert(dict_entity_try_reserve(entity_probe, 1024));
	assert(*dict_entity_get(entity_probe, 11) == &entity_probe_a);
	assert(*dict_entity_get(entity_probe, 22) == &entity_probe_b);
	/* Shrink allocates a complete replacement index before mutating the old one.
	 * Replacement OOM commits the erase but leaves the larger table valid; the
	 * next erase retries the optional shrink. */
	dict_entity_reset(entity_probe);
	for(uint32_t i = 0; i < 80; i++)
		*dict_entity_safe_get(entity_probe, i) = &entity_probe_a;
	uint32_t entity_erase_key = 0;
	while(dict_entity_size(entity_probe) > entity_probe->lower_limit)
		assert(dict_entity_erase(entity_probe, entity_erase_key++));
	assert(entity_probe->lower_limit > 0);
	size_t grown_entity_mask = entity_probe->mask;
	m_indexhash_t* grown_entity_index = entity_probe->index;
	dict_entity_test_fail_realloc_after(1); // replacement index
	assert(dict_entity_erase(entity_probe, entity_erase_key++));
	assert(entity_probe->mask == grown_entity_mask);
	assert(entity_probe->index == grown_entity_index);
	assert(entity_probe->lower_limit == entity_probe->count);
	for(uint32_t i = entity_erase_key; i < 80; i++)
		assert(*dict_entity_get(entity_probe, i) == &entity_probe_a);
	assert(dict_entity_erase(entity_probe, entity_erase_key++));
	assert(entity_probe->mask < grown_entity_mask);
	for(uint32_t i = entity_erase_key; i < 80; i++)
		assert(*dict_entity_get(entity_probe, i) == &entity_probe_a);

	dict_entity_clear(entity_probe);

	/* Generic sized allocators must see the exact physical count on every
	 * REALLOC/FREE. Shrink now performs one pre-mutation allocation; arming the
	 * second occurrence proves there is no hidden scratch+final-realloc pair. */
	sized_dict_realloc_calls = 0;
	sized_dict_realloc_fail_countdown = 0;
	sized_dict_count_mismatch = false;
	dict_sized_probe_t sized_probe;
	dict_sized_probe_init(sized_probe);
	for(uint32_t i = 0; i < 80; i++)
		*dict_sized_probe_safe_get(sized_probe, i) = i + 100;
	uint32_t sized_erase_key = 0;
	while(dict_sized_probe_size(sized_probe) > sized_probe->lower_limit)
		assert(dict_sized_probe_erase(sized_probe, sized_erase_key++));
	size_t sized_old_mask = sized_probe->mask;
	unsigned sized_calls_before = sized_dict_realloc_calls;
	sized_dict_realloc_fail_countdown = 2;
	assert(dict_sized_probe_erase(sized_probe, sized_erase_key++));
	assert(sized_dict_realloc_calls == sized_calls_before + 1);
	assert(sized_dict_realloc_fail_countdown == 1);
	sized_dict_realloc_fail_countdown = 0;
	assert(sized_probe->mask < sized_old_mask);
	assert(sized_probe->count_delete == sized_probe->count);
	for(uint32_t i = sized_erase_key; i < 80; i++)
		assert(*dict_sized_probe_get(sized_probe, i) == i + 100);
	dict_sized_probe_clear(sized_probe);
	assert(!sized_dict_count_mismatch);

	/* Exercise the same single pre-mutation failure edge through world.c's
	 * external allocator state, not only the header-local entity allocator. */
	for(int64_t i = 0; i < 80; i++) {
		struct world_section* section
			= dict_wsection_safe_get(gstate.world.sections, 100 + i);
		memset(section, 0, sizeof(*section));
		section->heightmap[0] = (uint8_t)(i + 1);
	}
	int64_t section_erase_key = 100;
	while(dict_wsection_size(gstate.world.sections)
		  > gstate.world.sections->lower_limit)
		assert(dict_wsection_erase(
			gstate.world.sections, section_erase_key++));
	assert(gstate.world.sections->lower_limit > 0);
	size_t grown_section_mask = gstate.world.sections->mask;
	m_indexhash_t* grown_section_index = gstate.world.sections->index;
	world_test_fail_section_dict_realloc_after(1);
	assert(dict_wsection_erase(gstate.world.sections, section_erase_key++));
	assert(gstate.world.sections->mask == grown_section_mask);
	assert(gstate.world.sections->index == grown_section_index);
	assert(gstate.world.sections->lower_limit == gstate.world.sections->count);
	for(int64_t key = section_erase_key; key < 180; key++)
		assert(dict_wsection_get(gstate.world.sections, key)->heightmap[0]
			   == (uint8_t)(key - 99));
	assert(dict_wsection_erase(gstate.world.sections, section_erase_key++));
	assert(gstate.world.sections->mask < grown_section_mask);
	dict_wsection_reset(gstate.world.sections);

	/* Exercise world.c's real insertion helper under tombstone-only pressure.
	 * Retain one live entry so same-size compaction has checked scratch to fail;
	 * neither failure nor success may grow the 804-byte-value table. */
	size_t section_tombstone_count
		= (size_t)gstate.world.sections->upper_limit - 1;
	assert(section_tombstone_count > 0);
	for(size_t i = 0; i < section_tombstone_count; i++) {
		struct world_section* section = dict_wsection_safe_get(
			gstate.world.sections, 1000 + (int64_t)i);
		section->heightmap[0] = (uint8_t)(i + 1);
	}
	for(size_t i = 1; i < section_tombstone_count; i++)
		assert(dict_wsection_erase(
			gstate.world.sections, 1000 + (int64_t)i));
	assert(dict_wsection_size(gstate.world.sections) == 1);
	assert((size_t)gstate.world.sections->count_delete + 1
		   >= (size_t)gstate.world.sections->upper_limit);
	struct dict_wsection_s section_before = *gstate.world.sections;
	world_test_fail_section_dict_realloc_after(1);
	world_set_block(
		&gstate.world, 512, 70, 512,
		(struct block_data) {.type = BLOCK_STONE}, false);
	assert(!world_find_chunk(&gstate.world, 512, 70, 512));
	assert(gstate.world.sections->data == section_before.data
		   && gstate.world.sections->index == section_before.index
		   && gstate.world.sections->mask == section_before.mask
		   && gstate.world.sections->count == section_before.count
		   && gstate.world.sections->count_delete == section_before.count_delete);
	assert(dict_wsection_get(gstate.world.sections, 1000)->heightmap[0] == 1);
	world_set_block(
		&gstate.world, 512, 70, 512,
		(struct block_data) {.type = BLOCK_STONE}, false);
	assert(world_find_chunk(&gstate.world, 512, 70, 512));
	assert(gstate.world.sections->mask == section_before.mask);
	assert(gstate.world.sections->count_delete
		   == gstate.world.sections->count);
	world_unload_all(&gstate.world);

	/* Live occupancy really does require growth at upper_limit. Its third
	 * allocation is the same checked scratch, and rollback must keep the original
	 * table byte-for-byte reachable before a successful retry grows it. */
	section_tombstone_count
		= (size_t)gstate.world.sections->upper_limit - 1;
	for(size_t i = 0; i < section_tombstone_count; i++) {
		struct world_section* section = dict_wsection_safe_get(
			gstate.world.sections, 2000 + (int64_t)i);
		section->heightmap[0] = (uint8_t)(i + 1);
	}
	section_before = *gstate.world.sections;
	world_test_fail_section_dict_realloc_after(3);
	world_set_block(
		&gstate.world, 512, 70, 512,
		(struct block_data) {.type = BLOCK_STONE}, false);
	assert(!world_find_chunk(&gstate.world, 512, 70, 512));
	assert(gstate.world.sections->data == section_before.data
		   && gstate.world.sections->index == section_before.index
		   && gstate.world.sections->mask == section_before.mask
		   && gstate.world.sections->count == section_before.count
		   && gstate.world.sections->count_delete == section_before.count_delete);
	assert(dict_wsection_get(gstate.world.sections, 2000)->heightmap[0] == 1);
	world_set_block(
		&gstate.world, 512, 70, 512,
		(struct block_data) {.type = BLOCK_STONE}, false);
	assert(world_find_chunk(&gstate.world, 512, 70, 512));
	assert(gstate.world.sections->mask > section_before.mask);
	world_unload_all(&gstate.world);

	/* The far bulk builder has the same dictionary admission boundary, but also
	 * owns private replacement chunks by the time admission runs. A failed grow
	 * must release those chunks and leave every published section untouched. */
	section_tombstone_count
		= (size_t)gstate.world.sections->upper_limit - 1;
	for(size_t i = 0; i < section_tombstone_count; i++) {
		struct world_section* section = dict_wsection_safe_get(
			gstate.world.sections, 3000 + (int64_t)i);
		section->heightmap[0] = (uint8_t)(i + 1);
	}
	section_before = *gstate.world.sections;
	uint8_t far_skin[CHUNK_SIZE * CHUNK_SIZE];
	uint8_t far_top[CHUNK_SIZE * CHUNK_SIZE];
	uint8_t far_grass_i[CHUNK_SIZE * CHUNK_SIZE] = {0};
	uint8_t far_grass_j[CHUNK_SIZE * CHUNK_SIZE] = {0};
	memset(far_skin, 70, sizeof(far_skin));
	memset(far_top, BLOCK_STONE, sizeof(far_top));
	world_test_fail_section_dict_realloc_after(3);
	assert(!world_build_far_section(
		&gstate.world, 768, 768, far_skin, far_top, far_grass_i,
		far_grass_j));
	assert(!world_find_chunk(&gstate.world, 768, 70, 768));
	assert(gstate.world.sections->data == section_before.data
		   && gstate.world.sections->index == section_before.index
		   && gstate.world.sections->mask == section_before.mask
		   && gstate.world.sections->count == section_before.count
		   && gstate.world.sections->count_delete == section_before.count_delete);
	assert(dict_wsection_get(gstate.world.sections, 3000)->heightmap[0] == 1);
	assert(world_build_far_section(
		&gstate.world, 768, 768, far_skin, far_top, far_grass_i,
		far_grass_j));
	assert(world_find_chunk(&gstate.world, 768, 70, 768));
	assert(gstate.world.sections->mask > section_before.mask);
	/* Replacing an existing non-air owner follows a different commit path from
	 * first admission. Hold one reference so pointer identity remains valid while
	 * proving the complete replacement was published only after allocation. */
	struct chunk* prior_far = world_find_chunk(&gstate.world, 768, 70, 768);
	assert(prior_far);
	chunk_ref(prior_far);
	far_top[0] = BLOCK_DIRT;
	assert(world_build_far_section(
		&gstate.world, 768, 768, far_skin, far_top, far_grass_i,
		far_grass_j));
	assert(world_find_chunk(&gstate.world, 768, 70, 768) != prior_far);
	assert(world_get_block(&gstate.world, 768, 70, 768).type == BLOCK_DIRT);
	chunk_unref(prior_far);
	/* A complete no-surface skin is an intentional successful replacement, not
	 * an allocation failure. It must remove the old owner without asking stream
	 * recovery to resend a section that is authoritatively all air. */
	memset(far_skin, 0xFF, sizeof(far_skin));
	assert(world_build_far_section(
		&gstate.world, 768, 768, far_skin, far_top, far_grass_i,
		far_grass_j));
	assert(!world_find_chunk(&gstate.world, 768, 70, 768));
	world_unload_all(&gstate.world);
	particle_init();
	struct client_rpc reset = {
		.type = CRPC_WORLD_RESET,
		.payload.world_reset = {
			.dimension = WORLD_DIM_OVERWORLD,
			.local_entity = 0,
		},
	};
	clin_process(&reset);
	assert(gstate.players[0].local_player && gstate.players[0].windows[WINDOWC_INVENTORY]);
	/* A marker whose center section is absent must fail closed and request a
	 * restream for the exact epoch; it may not expose an empty collision world. */
	struct client_rpc missing_center = {
		.type = CRPC_PLAYER_POS,
		.payload.player_pos = {
			.position = {36.5f, 70.0f, 40.5f},
			.input_epoch = 90,
			.oxygen = MAX_OXYGEN,
			.selected_hotbar = 0,
		},
	};
	clin_process(&missing_center);
	assert(!gstate.world_loaded && gstate.world_switch_pending);
	struct input_gate_capture retry_capture = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &retry_capture, false);
	assert(retry_capture.count == 1);
	assert(retry_capture.ack_index == 0 && retry_capture.ack_epoch == 90);
	assert(retry_capture.retry);
	/* Exhausted recovery is visibly terminal, while a genuinely new WORLD_RESET
	 * owns the reset semantics and clears the prior session's error. */
	struct client_rpc load_failed = {.type = CRPC_LOAD_FAILED};
	clin_process(&load_failed);
	assert(gstate.world_load_failed);
	assert(!gstate.world_loaded && gstate.world_switch_pending);
	assert(gstate.players[0].current_screen == &screen_load_world);
	clin_process(&reset);
	assert(!gstate.world_load_failed);
	assert(gstate.players[0].local_player && gstate.players[0].windows[WINDOWC_INVENTORY]);
	world_set_block(
		&gstate.world, 4, 70, 8,
		(struct block_data) {.type = BLOCK_WATER_STILL}, false);
	/* Center-first streaming used to hide a later outer-column OOM: the center
	 * survived, the marker ACKed, and the server retained `sent=true` forever for
	 * the dropped column. Fail a real non-center full-section allocation and prove
	 * the sticky stream error requests a clean restream before any ACK/exposure. */
	chunk_test_fail_alloc_after(1);
	struct chunk_rpc_slot* failed_outer_slot = clin_chunk_slot_acquire();
	struct client_rpc failed_outer = {
		.type = CRPC_CHUNK,
		.payload.chunk = {
			.x = 32,
			.y = 0,
			.z = 32,
			.sx = CHUNK_SIZE,
			.sy = WORLD_HEIGHT,
			.sz = CHUNK_SIZE,
			.slot = failed_outer_slot,
		},
	};
	clin_process(&failed_outer);
	assert(world_find_chunk(&gstate.world, 4, 70, 8));
	assert(!world_find_chunk(&gstate.world, 32, 70, 32));
	struct client_rpc failed_outer_marker = {
		.type = CRPC_PLAYER_POS,
		.payload.player_pos = {
			.position = {4.5f, 70.0f, 8.5f},
			.input_epoch = 91,
			.oxygen = MAX_OXYGEN,
			.selected_hotbar = 0,
		},
	};
	clin_process(&failed_outer_marker);
	assert(!gstate.world_loaded && gstate.world_switch_pending);
	assert(world_loaded_chunks(&gstate.world) == 0);
	struct input_gate_capture outer_retry = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &outer_retry, false);
	assert(outer_retry.count == 1 && outer_retry.retry);
	assert(outer_retry.ack_epoch == 91);
	/* Simulate the server's clean restream; no second WORLD_RESET is sent. */
	world_set_block(
		&gstate.world, 4, 70, 8,
		(struct block_data) {.type = BLOCK_WATER_STILL}, false);
	gstate.players[0].camera.x = 900.0f;
	gstate.players[0].camera.y = 12.0f;
	gstate.players[0].camera.z = -900.0f;
	gstate.players[0].local_player->vel[0] = 9.0f;
	gstate.players[0].local_player->vel[1] = 9.0f;
	gstate.players[0].local_player->vel[2] = 9.0f;
	gstate.players[0].local_player->orient[0] = 2.5f;
	gstate.players[0].local_player->orient[1] = 2.0f;
	gstate.players[0].local_player->orient_old[0] = -2.5f;
	gstate.players[0].local_player->orient_old[1] = -2.0f;
	gstate.players[0].local_player->data.local_player.jump_ticks = 4;
	gstate.players[0].local_player->on_fire = false;
	gstate.players[0].in_water = false;
	gstate.players[0].oxygen = 0;
	gstate.world_time = 999;
	gstate.world_time_start = time_add_ms(time_get(), 60000);
	ptime_t held_time_start = gstate.world_time_start;
	inventory_set_hotbar(
		windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]), 1);
	gstate.players[0].digging.active = true;
	gstate.players[0].held_item_animation.punch.place = true;
	gstate.players[0].camera_hit.hit = true;
	gstate.players[0].camera_hit.entity_hit = true;
	gstate.players[0].camera_hit.entity_id = 123;
	gstate.autopilot.pending_place = 2;
	for(int i = 0; i < 16; i++) {
		struct server_rpc pos = {.type = SRPC_PLAYER_POS};
		assert(svin_rpc_send(&pos));
	}
	for(int i = 0; i < 64; i++) {
		struct server_rpc dig = {
			.type = SRPC_BLOCK_DIG,
			.payload.block_dig = {.x = i, .y = 64, .z = 0},
		};
		assert(svin_rpc_send(&dig));
	}
	for(uint8_t window = 1; window <= 3; window++) {
		struct server_rpc close = {
			.type = SRPC_WINDOW_CLOSE,
			.payload.window_close.window = window,
		};
		assert(svin_rpc_send(&close));
	}
	struct client_rpc handoff = {
		.type = CRPC_PLAYER_POS,
		.payload.player_pos = {
			.position = {4.5f, 70.0f, 8.5f},
			.rotation = {45.0f, -15.0f},
			.velocity = {1.0f, 2.0f, 3.0f},
			.world_time = 4242,
			.input_epoch = 92,
			.oxygen = 77,
			.selected_hotbar = 6,
			.on_fire = true,
			.fresh_body = false,
		},
	};
	clin_process(&handoff);
	/* Installed snapshots do not acknowledge a view with missing terrain. */
	assert(svin_pending_count() == 67);
	assert(!svin_input_ready_pending(92));
	assert(gstate.world_switch_pending);
	assert(gstate.players[0].camera.x == 4.5f && gstate.players[0].camera.y == 70.0f
		   && gstate.players[0].camera.z == 8.5f);
	/* WORLD_RESET constructed a zeroed destination body. The consolidated marker
	 * restores the server-carried physics/air/fire/hotbar snapshot while dropping
	 * every input-owned transient from the source epoch before ACK. */
	assert(gstate.players[0].local_player->vel[0] == 1.0f
		   && gstate.players[0].local_player->vel[1] == 2.0f
		   && gstate.players[0].local_player->vel[2] == 3.0f);
	float expected_rx = glm_rad(-45.0f);
	float expected_ry = glm_rad(75.0f);
	assert(fabsf(gstate.players[0].local_player->orient[0] - expected_rx) < 1.0e-6f
		   && fabsf(gstate.players[0].local_player->orient[1] - expected_ry) < 1.0e-6f);
	assert(fabsf(gstate.players[0].local_player->orient_old[0] - expected_rx) < 1.0e-6f
		   && fabsf(gstate.players[0].local_player->orient_old[1] - expected_ry) < 1.0e-6f);
	assert(gstate.players[0].local_player->data.local_player.jump_ticks == 4);
	assert(gstate.players[0].local_player->on_fire);
	assert(gstate.players[0].in_water);
	assert(gstate.players[0].oxygen == 77);
	assert(gstate.world_time == 4242);
	assert(memcmp(&gstate.world_time_start, &held_time_start,
			  sizeof(held_time_start)) == 0);
	assert(inventory_get_hotbar(windowc_get_latest(
			   gstate.players[0].windows[WINDOWC_INVENTORY])) == 6);
	assert(!gstate.players[0].digging.active);
	assert(!gstate.players[0].held_item_animation.punch.place);
	assert(!gstate.players[0].camera_hit.hit && !gstate.players[0].camera_hit.entity_hit
		   && gstate.players[0].camera_hit.entity_id == 0);
	assert(gstate.autopilot.pending_place == 0);
	/* A frame with a still-full server inbox must not reveal input merely because
	 * the ACK was accepted into its dedicated tail slot. */
	clin_update();
	assert(gstate.world_switch_pending);
	assert(!svin_input_ready_pending(92));
	assert(memcmp(&gstate.world_time_start, &held_time_start,
			  sizeof(held_time_start)) == 0);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	assert(gstate.players[0].current_screen == &screen_load_world);
	assert(gstate.world_switch_pending && svin_input_ready_pending(92));
	/* The prepared fallback stays latched while its ACK waits in the FIFO. */
	assert(svin_pending_count() == 68);
	assert(!game_state_client_entities_active(&gstate));
	vec3 held_pos, held_vel;
	glm_vec3_copy(gstate.players[0].local_player->pos, held_pos);
	glm_vec3_copy(gstate.players[0].local_player->vel, held_vel);
	/* This is the exact guard used by main's 20 Hz entity tick. A queued ACK must
	 * freeze even a falling body instead of advancing it behind the load screen. */
	if(game_state_client_entities_active(&gstate))
		entities_client_tick(gstate.entities);
	assert(memcmp(gstate.players[0].local_player->pos, held_pos, sizeof(held_pos)) == 0);
	assert(memcmp(gstate.players[0].local_player->vel, held_vel, sizeof(held_vel)) == 0);

	struct input_gate_capture capture = {
		.ack_index = SIZE_MAX,
	};
	while(svin_pending_count() > 0) {
		svin_process_messages(capture_input_gate_transport, &capture, false);
		svin_rpc_flush_pending();
	}
	/* The final flush may leave the tail in the live inbox. */
	svin_process_messages(capture_input_gate_transport, &capture, false);
	assert(!svin_input_ready_pending(92));
	assert(capture.count == 84);
	assert(capture.ack_index == 83 && capture.ack_epoch == 92);
	assert(!capture.retry);
	clin_update();
	assert(!gstate.world_switch_pending);
	assert(gstate.players[0].current_screen == &screen_ingame);
	assert(lworld_load_screen_ms_result() == 30000);
	puts("R1 PASS: presentation precedes ACK; queued ACK retains input hold");
	assert(game_state_client_entities_active(&gstate));
	int32_t rebase_age_ms = time_diff_ms(gstate.world_time_start, time_get());
	assert(rebase_age_ms >= 0 && rebase_age_ms < 1000);
	/* Input exposure emits the first reliable movement-state snapshot. Drain and
	 * pin it here so the duplicate-marker check below remains about stale marker
	 * traffic only. */
	struct input_gate_capture movement_state = {.ack_index = SIZE_MAX};
	svin_process_messages(
		capture_input_gate_transport, &movement_state, false);
	assert(movement_state.count == 1
		   && movement_state.player_state_count == 1
		   && !movement_state.player_on_ground
		   && !movement_state.player_sneaking
		   && movement_state.player_correction_seq == 0);
	/* An untagged delayed GUI cancellation cannot overwrite a newer accepted
	 * optimistic pause or pull its screen back to gameplay. */
	gstate.paused = true;
	gstate.players[0].current_screen = &screen_load_world;
	gstate.players[0].visible_window = WINDOWC_CHEST;
	gstate.players[0].deferred_reopen_window = WINDOWC_CHEST;
	gstate.players[0].menu_host.mode = LOCAL_MENU_CONTAINER;
	struct client_rpc delayed_return = {.type = CRPC_RETURN_INGAME};
	clin_process(&delayed_return);
	assert(!local_menu_host_active(&gstate.players[0]));
	assert(gstate.players[0].visible_window == -1
		   && gstate.players[0].deferred_reopen_window == -1);
	assert(gstate.paused && gstate.players[0].current_screen == &screen_load_world);
	gstate.paused = false;
	screen_set(&screen_ingame);

	/* A duplicate or older nonzero marker is not merely an idempotent teleport:
	 * it carries body, hotbar and ACK ownership. Reject it before any of those
	 * fields or the handoff latches can change. */
	float stable_camera_x = gstate.players[0].camera.x;
	vec3 stable_body_pos;
	glm_vec3_copy(gstate.players[0].local_player->pos, stable_body_pos);
	size_t stable_hotbar = inventory_get_hotbar(
		windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]));
	struct client_rpc duplicate_marker = handoff;
	duplicate_marker.payload.player_pos.position[0] = 777.0f;
	duplicate_marker.payload.player_pos.selected_hotbar = 2;
	clin_process(&duplicate_marker); /* epoch 92 again */
	struct client_rpc stale_marker = duplicate_marker;
	stale_marker.payload.player_pos.input_epoch = 91;
	stale_marker.payload.player_pos.position[0] = -777.0f;
	clin_process(&stale_marker);
	assert(gstate.players[0].camera.x == stable_camera_x);
	assert(memcmp(gstate.players[0].local_player->pos, stable_body_pos,
			  sizeof(stable_body_pos)) == 0);
	assert(inventory_get_hotbar(windowc_get_latest(
			   gstate.players[0].windows[WINDOWC_INVENTORY])) == stable_hotbar);
	assert(!gstate.world_switch_pending && svin_pending_count() == 0);
	struct input_gate_capture stale_transport = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &stale_transport, false);
	assert(stale_transport.count == 0);

	/* Invalid marker payloads must issue an exact NACK instead of silently
	 * stranding the server's input barrier. No snapshot field is installed. */
	struct client_rpc invalid_hotbar = handoff;
	invalid_hotbar.payload.player_pos.input_epoch = 93;
	invalid_hotbar.payload.player_pos.position[0] = 333.0f;
	invalid_hotbar.payload.player_pos.selected_hotbar = INVENTORY_SIZE_HOTBAR;
	clin_process(&invalid_hotbar);
	assert(gstate.players[0].camera.x == stable_camera_x);
	assert(!gstate.world_loaded && gstate.world_switch_pending);
	struct input_gate_capture invalid_hotbar_capture = {.ack_index = SIZE_MAX};
	svin_process_messages(
		capture_input_gate_transport, &invalid_hotbar_capture, false);
	assert(invalid_hotbar_capture.count == 1
		   && invalid_hotbar_capture.ack_epoch == 93
		   && invalid_hotbar_capture.retry);
	struct client_rpc after_invalid = handoff;
	after_invalid.payload.player_pos.input_epoch = 94;
	clin_process(&after_invalid);
	assert(gstate.world_loaded && gstate.world_switch_pending);
	struct input_gate_capture before_mesh_ack = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &before_mesh_ack, false);
	assert(before_mesh_ack.count == 0);
	assert(gstate.players[0].current_screen == &screen_load_world);
	/* No ACK: install every neighbor but leave the meshes pending. */
	for(int cx = -MAX_VIEW_DISTANCE; cx <= MAX_VIEW_DISTANCE; cx++)
		for(int cz = -MAX_VIEW_DISTANCE; cz <= MAX_VIEW_DISTANCE; cz++)
			world_set_block(&gstate.world, cx * CHUNK_SIZE + 4, 70,
				cz * CHUNK_SIZE + 8,
				(struct block_data) {.type = BLOCK_WATER_STILL}, false);
	screen_load_world.update(&screen_load_world, 1.0f);
	clin_update_input_readiness();
	assert(gstate.players[0].current_screen == &screen_load_world);
	/* Supply settled mesher state through the real world metadata. */
	dict_wsection_it_t ready_it;
	for(dict_wsection_it(ready_it, gstate.world.sections);
		!dict_wsection_end_p(ready_it); dict_wsection_next(ready_it)) {
		struct world_section* section = &dict_wsection_ref(ready_it)->value;
		for(int cy = 0; cy < COLUMN_HEIGHT; cy++)
			if(section->column[cy]) {
				section->column[cy]->rebuild_displist = false;
				section->column[cy]->first_meshed = true;
			}
	}
	struct chunk* pending_mesh = world_find_chunk(&gstate.world, 4, 70, 8);
	assert(pending_mesh);
	pending_mesh->mesh_in_flight = true;
	screen_load_world.update(&screen_load_world, 1.0f);
	clin_update_input_readiness();
	assert(gstate.players[0].current_screen == &screen_load_world);
	struct input_gate_capture in_flight_ack = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &in_flight_ack, false);
	assert(in_flight_ack.count == 0 && gstate.world_switch_pending);
	pending_mesh->mesh_in_flight = false;
	screen_load_world.update(&screen_load_world, 1.0f);
	clin_update_input_readiness();
	assert(gstate.players[0].current_screen == &screen_ingame);
	assert(lworld_load_screen_ms_result() == 3000);
	struct input_gate_capture after_invalid_ack = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &after_invalid_ack, false);
	assert(after_invalid_ack.count == 1 && after_invalid_ack.ack_epoch == 94
		   && !after_invalid_ack.retry && !gstate.world_switch_pending);
	puts("R1 PASS: no ACK until installed and meshed neighbors are ready");
	for(int cx = -MAX_VIEW_DISTANCE; cx <= MAX_VIEW_DISTANCE; cx++)
		for(int cz = -MAX_VIEW_DISTANCE; cz <= MAX_VIEW_DISTANCE; cz++)
			if(cx != 0 || cz != 0)
				world_unload_section(&gstate.world, cx, cz);

	/* Post-ACK far-column OOM recovery keeps the section being replaced, all other
	 * voxels and every non-voxel owner until the dedicated FIFO RESET. A paused
	 * owner remains paused through the new marker/ACK rather than deadlocking the
	 * server stream. */
	world_set_block(
		&gstate.world, 4, 70, 8,
		(struct block_data) {.type = BLOCK_WATER_STILL}, false);
	world_set_block(
		&gstate.world, 64, 70, 64,
		(struct block_data) {.type = BLOCK_WATER_STILL}, false);
	struct chunk* retained_far_owner
		= world_find_chunk(&gstate.world, 64, 70, 64);
	assert(retained_far_owner);
	struct entity* recovery_player = gstate.players[0].local_player;
	struct window_container* recovery_inventory
		= gstate.players[0].windows[WINDOWC_INVENTORY];
	size_t recovery_entity_count = dict_entity_size(gstate.entities);
	enum world_dim recovery_dimension = gstate.world.dimension;
	gstate.paused = true;
	screen_set(&screen_pause);
	chunk_test_fail_alloc_after(2);
	struct client_rpc live_failed = client_far_chunk_rpc(64, 64);
	clin_process(&live_failed);
	assert_far_slot_pool_fully_available();
	assert(!gstate.world_loaded && gstate.world_switch_pending && gstate.paused);
	assert(gstate.players[0].current_screen == &screen_load_world);
	assert(world_find_chunk(&gstate.world, 64, 70, 64)
		   == retained_far_owner);
	assert(world_get_block(&gstate.world, 64, 70, 64).type
		   == BLOCK_WATER_STILL);
	assert(world_get_block(&gstate.world, 4, 70, 8).type
		   == BLOCK_WATER_STILL);
	struct input_gate_capture live_recover = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &live_recover, false);
	assert(live_recover.count == 1 && live_recover.recover_count == 1
		   && live_recover.recover_epoch == 94);
	struct client_rpc recovery_workbench = {
		.type = CRPC_OPEN_WINDOW,
		.payload.window_open = {
			.window = WINDOWC_CRAFTING,
			.type = WINDOW_TYPE_WORKBENCH,
			.slot_count = CRAFTING_SIZE,
		},
	};
	struct client_rpc recovery_reopen = {
		.type = CRPC_REOPEN_WINDOW,
		.payload.window_reopen = {
			.window = WINDOWC_CRAFTING,
			.type = WINDOW_TYPE_WORKBENCH,
			.slot_count = CRAFTING_SIZE,
		},
	};
	/* A queued OPEN that crosses a newer HOME pause cannot remain hidden on the
	 * server. Reject it reliably behind STREAM_RECOVER; an untagged RETURN in the
	 * same FIFO window must not erase the newer pause owner. */
	clin_process(&recovery_workbench);
	assert(!gstate.players[0].windows[WINDOWC_CRAFTING]);
	struct input_gate_capture paused_open_close = {.ack_index = SIZE_MAX};
	svin_process_messages(
		capture_input_gate_transport, &paused_open_close, false);
	assert(paused_open_close.count == 1 && paused_open_close.close_count == 1
		   && paused_open_close.close_window == WINDOWC_CRAFTING);
	struct client_rpc recovery_return = {.type = CRPC_RETURN_INGAME};
	clin_process(&recovery_return);
	assert(gstate.paused && gstate.players[0].current_screen == &screen_load_world);
	/* If that CLOSE failed, REOPEN rebuilds ownership but remains deferred behind
	 * both the recovery load screen and the authoritative pause. */
	clin_process(&recovery_reopen);
	assert(gstate.players[0].windows[WINDOWC_CRAFTING]
		   && gstate.players[0].current_screen == &screen_load_world);

	/* Messages already queued before server acceptance cannot mutate the retained
	 * old view. Pooled near/far payloads are still released. */
	struct client_rpc skipped_chunk = client_full_chunk_rpc(80, 80);
	clin_process(&skipped_chunk);
	struct far_rpc_slot* skipped_far_slot = clin_far_slot_try_acquire();
	assert(skipped_far_slot);
	memset(skipped_far_slot, 0, sizeof(*skipped_far_slot));
	struct client_rpc skipped_far = {
		.type = CRPC_FAR_CHUNK,
		.payload.far_chunk = {
			.x = 96,
			.z = 96,
			.slot = skipped_far_slot,
		},
	};
	clin_process(&skipped_far);
	assert_far_slot_pool_fully_available();
	struct client_rpc skipped_set = {
		.type = CRPC_SET_BLOCK,
		.payload.set_block = {
			.x = 4,
			.y = 70,
			.z = 8,
			.block = {.type = BLOCK_STONE},
		},
	};
	clin_process(&skipped_set);
	struct client_rpc skipped_unload = {
		.type = CRPC_UNLOAD_CHUNK,
		.payload.unload_chunk = {.x = 0, .z = 0},
	};
	clin_process(&skipped_unload);
	assert(world_get_block(&gstate.world, 4, 70, 8).type
		   == BLOCK_WATER_STILL);
	assert(!world_find_chunk(&gstate.world, 80, 70, 80));
	assert(!world_find_chunk(&gstate.world, 96, 70, 96));

	struct client_rpc stream_reset = {.type = CRPC_STREAM_RESET};
	clin_process(&stream_reset);
	assert(world_loaded_chunks(&gstate.world) == 0);
	assert(gstate.players[0].local_player == recovery_player);
	assert(gstate.players[0].windows[WINDOWC_INVENTORY] == recovery_inventory);
	assert(dict_entity_size(gstate.entities) == recovery_entity_count);
	assert(gstate.world.dimension == recovery_dimension && gstate.paused);
	struct client_rpc recovered_center = client_full_chunk_rpc(0, 0);
	clin_process(&recovered_center);
	assert(world_find_chunk(&gstate.world, 4, 70, 8));
	struct client_rpc pause_recovered = handoff;
	pause_recovered.payload.player_pos.input_epoch = 95;
	clin_process(&pause_recovered);
	assert(gstate.world_switch_pending && gstate.paused);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	assert(gstate.world_loaded && !gstate.world_switch_pending && gstate.paused);
	assert(gstate.players[0].current_screen == &screen_pause);
	struct input_gate_capture pause_recovered_ack = {.ack_index = SIZE_MAX};
	svin_process_messages(
		capture_input_gate_transport, &pause_recovered_ack, false);
	assert(pause_recovered_ack.count == 1
		   && pause_recovered_ack.ack_epoch == 95
		   && !pause_recovered_ack.retry);
	gstate.paused = false;
	screen_set(&screen_ingame);
	assert(gstate.players[0].current_screen == &screen_ingame);
	clin_update();
	assert(gstate.players[0].current_screen == &screen_crafting);
	struct input_gate_capture resumed_pause_transport = {.ack_index = SIZE_MAX};
	svin_process_messages(
		capture_input_gate_transport, &resumed_pause_transport, false);

	/* The same voxel-only cycle retains and restores the exact active container
	 * id. It never destroys/recreates the local window or silently hides the
	 * still-authoritative server inventory behind gameplay. */
	gstate.paused = false;
	screen_set(&screen_ingame);
	clin_process(&recovery_workbench);
	assert(gstate.players[0].current_screen == &screen_crafting);
	struct window_container* fixed_workbench
		= gstate.players[0].windows[WINDOWC_CRAFTING];
	chunk_test_fail_alloc_after(1);
	struct client_rpc gui_failed = client_full_chunk_rpc(112, 112);
	clin_process(&gui_failed);
	assert(gstate.players[0].current_screen == &screen_load_world
		   && gstate.players[0].windows[WINDOWC_CRAFTING] == fixed_workbench);
	struct input_gate_capture gui_recover = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &gui_recover, false);
	assert(gui_recover.count == 1 && gui_recover.recover_count == 1
		   && gui_recover.recover_epoch == 95);
	/* OPEN/REOPEN may already be in the opposite-direction FIFO when the failed
	 * CHUNK is consumed. Rebuild/update the exact owner while keeping the loading
	 * screen; no hidden server container and no premature GUI exposure. */
	clin_process(&recovery_workbench);
	fixed_workbench = gstate.players[0].windows[WINDOWC_CRAFTING];
	assert(fixed_workbench && gstate.players[0].current_screen == &screen_load_world);
	clin_process(&recovery_reopen);
	assert(gstate.players[0].windows[WINDOWC_CRAFTING] == fixed_workbench
		   && gstate.players[0].current_screen == &screen_load_world);
	clin_process(&stream_reset);
	assert(gstate.players[0].windows[WINDOWC_CRAFTING] == fixed_workbench
		   && gstate.players[0].local_player == recovery_player);
	recovered_center = client_full_chunk_rpc(0, 0);
	clin_process(&recovered_center);
	struct client_rpc gui_recovered = handoff;
	gui_recovered.payload.player_pos.input_epoch = 96;
	clin_process(&gui_recovered);
	assert(gstate.world_switch_pending);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	assert(gstate.world_loaded && !gstate.world_switch_pending);
	assert(gstate.players[0].current_screen == &screen_crafting
		   && gstate.players[0].windows[WINDOWC_CRAFTING] == fixed_workbench);
	struct input_gate_capture gui_recovered_ack = {.ack_index = SIZE_MAX};
	svin_process_messages(
		capture_input_gate_transport, &gui_recovered_ack, false);
	assert(gui_recovered_ack.count == 1 && gui_recovered_ack.ack_epoch == 96);

	/* A RETURN queued after a captured unpaused window means that exact owner is
	 * gone. It keeps recovery gated but selects ingame after the replacement ACK. */
	chunk_test_fail_alloc_after(1);
	struct client_rpc return_failed = client_full_chunk_rpc(128, 128);
	clin_process(&return_failed);
	struct input_gate_capture return_recover = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &return_recover, false);
	assert(return_recover.recover_count == 1
		   && return_recover.recover_epoch == 96);
	clin_process(&recovery_return);
	clin_process(&stream_reset);
	recovered_center = client_full_chunk_rpc(0, 0);
	clin_process(&recovered_center);
	struct client_rpc return_recovered = handoff;
	return_recovered.payload.player_pos.input_epoch = 97;
	clin_process(&return_recovered);
	assert(gstate.world_loaded && gstate.world_switch_pending
		   && !gstate.paused && gstate.players[0].current_screen == &screen_load_world);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	assert(gstate.players[0].current_screen == &screen_ingame);
	struct input_gate_capture return_recovered_ack = {.ack_index = SIZE_MAX};
	svin_process_messages(
		capture_input_gate_transport, &return_recovered_ack, false);
	assert(return_recovered_ack.ack_epoch == 97);

	/* If a reversible BEGIN wins the cross-direction race, it invalidates the
	 * recovery request but not the known voxel hole. A stale marker cannot mutate
	 * the new barrier, and the same-world cancel marker must NACK/restream rather
	 * than blessing the retained center plus a failed outer column. */
	chunk_test_fail_alloc_after(1);
	struct client_rpc begin_race_failed = client_full_chunk_rpc(144, 144);
	clin_process(&begin_race_failed);
	struct input_gate_capture begin_race_request = {.ack_index = SIZE_MAX};
	svin_process_messages(
		capture_input_gate_transport, &begin_race_request, false);
	assert(begin_race_request.recover_epoch == 97);
	struct client_rpc begin_race = {.type = CRPC_BEGIN_WORLD_SWITCH};
	clin_process(&begin_race);
	float begin_camera_x = gstate.players[0].camera.x;
	struct client_rpc stale_during_begin = handoff;
	stale_during_begin.payload.player_pos.input_epoch = 96;
	stale_during_begin.payload.player_pos.position[0] = 606.0f;
	clin_process(&stale_during_begin);
	assert(gstate.players[0].camera.x == begin_camera_x && gstate.world_switch_pending);
	struct client_rpc cancel_after_failed_stream = handoff;
	cancel_after_failed_stream.payload.player_pos.input_epoch = 98;
	clin_process(&cancel_after_failed_stream);
	assert(!gstate.world_loaded && gstate.world_switch_pending
		   && world_loaded_chunks(&gstate.world) == 0);
	struct input_gate_capture cancel_stream_retry = {.ack_index = SIZE_MAX};
	svin_process_messages(
		capture_input_gate_transport, &cancel_stream_retry, false);
	assert(cancel_stream_retry.ack_epoch == 98
		   && cancel_stream_retry.retry);
	recovered_center = client_full_chunk_rpc(0, 0);
	clin_process(&recovered_center);
	struct client_rpc cancel_stream_recovered = handoff;
	cancel_stream_recovered.payload.player_pos.input_epoch = 99;
	clin_process(&cancel_stream_recovered);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	struct input_gate_capture cancel_stream_ack = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &cancel_stream_ack, false);
	assert(cancel_stream_ack.ack_epoch == 99 && !cancel_stream_ack.retry
		   && !gstate.world_switch_pending);

	/* Serial-number wrap remains forward progress (UINT32_MAX -> 1), while the
	 * just-superseded maximum token is stale after wrap. This seam models a fresh
	 * process/server fixture; production never resets the guard mid-session. */
	clin_test_reset_input_epoch_guard();
	struct client_rpc wrap_max = handoff;
	wrap_max.payload.player_pos.input_epoch = UINT32_MAX;
	wrap_max.payload.player_pos.position[0] = 4.25f;
	clin_process(&wrap_max);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	struct input_gate_capture wrap_max_ack = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &wrap_max_ack, false);
	assert(wrap_max_ack.ack_epoch == UINT32_MAX);
	struct client_rpc wrap_one = handoff;
	wrap_one.payload.player_pos.input_epoch = 1;
	wrap_one.payload.player_pos.position[0] = 4.75f;
	clin_process(&wrap_one);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	struct input_gate_capture wrap_one_ack = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &wrap_one_ack, false);
	assert(wrap_one_ack.ack_epoch == 1 && gstate.players[0].camera.x == 4.75f);
	wrap_max.payload.player_pos.position[0] = 999.0f;
	clin_process(&wrap_max);
	assert(gstate.players[0].camera.x == 4.75f && !gstate.world_switch_pending);
	/* A newer BEGIN already in the reply queue must retire an installed marker
	 * still waiting for meshes before the next frame tries to ACK it. */
	clin_process(&begin_race);
	struct client_rpc waiting_marker = handoff;
	waiting_marker.payload.player_pos.input_epoch = 2;
	clin_process(&waiting_marker);
	assert(gstate.world_switch_pending && !svin_input_ready_pending(2));
	struct client_rpc premature_return = {.type = CRPC_RETURN_INGAME};
	clin_process(&premature_return);
	assert(gstate.players[0].current_screen == &screen_load_world);
	clin_rpc_send(&begin_race);
	clin_update();
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	struct input_gate_capture retired_marker = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &retired_marker, false);
	assert(retired_marker.count == 0 && gstate.world_switch_pending);

	/* Continued loading can fail AFTER the marker but BEFORE its ACK. Retire
	 * that exact epoch with a NACK and preserve non-voxel owners for restream. */
	screen_set(&screen_load_world);
	waiting_marker.payload.player_pos.input_epoch = 3;
	clin_process(&waiting_marker);
	assert(gstate.world_switch_pending);
	chunk_test_fail_alloc_after(1);
	struct client_rpc late_install_failure = client_full_chunk_rpc(160, 160);
	clin_process(&late_install_failure);
	clin_update_input_readiness();
	struct input_gate_capture late_install_retry = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &late_install_retry, false);
	assert(late_install_retry.count == 1 && late_install_retry.ack_epoch == 3
		   && late_install_retry.retry && !gstate.world_loaded
		   && gstate.world_switch_pending && world_loaded_chunks(&gstate.world) == 0);
	assert(gstate.players[0].local_player == recovery_player);
	assert(gstate.players[0].windows[WINDOWC_INVENTORY] == recovery_inventory);

	/* This primary local-world suspension is not imposed on a remote server.
	 * Its position acknowledgement keeps flowing while its local view waits. */
	recovered_center = client_full_chunk_rpc(0, 0);
	clin_process(&recovered_center);
	gstate.remote_session = true;
	waiting_marker.payload.player_pos.input_epoch = 4;
	clin_process(&waiting_marker);
	struct input_gate_capture remote_ack = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &remote_ack, false);
	assert(remote_ack.count == 1 && remote_ack.ack_epoch == 4 && !remote_ack.retry);
	assert(!gstate.world_switch_pending
		   && gstate.players[0].current_screen == &screen_load_world);
	gstate.remote_session = false;

	clin_process(&begin_race);
	waiting_marker.payload.player_pos.input_epoch = 5;
	clin_process(&waiting_marker);
	assert(gstate.world_switch_pending);
	clin_process(&load_failed);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	struct input_gate_capture failed_marker = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &failed_marker, false);
	assert(failed_marker.count == 0 && gstate.world_load_failed
		   && gstate.world_switch_pending);
	puts("R1 PASS: queued BEGIN/failure retire readiness; late install NACK; remote unchanged");

	/* A newer valid marker may supersede an ACK already queued behind pressure.
	 * Draining that older ACK must not expose/rebase the newer waiting snapshot,
	 * even without a separate BEGIN between the two accepted markers. */
	clin_process(&reset);
	recovered_center = client_full_chunk_rpc(0, 0);
	clin_process(&recovered_center);
	for(int i = 0; i < 16; i++) {
		struct server_rpc pressure = {.type = SRPC_PLAYER_POS};
		assert(svin_rpc_send(&pressure));
	}
	waiting_marker.payload.player_pos.input_epoch = 6;
	clin_process(&waiting_marker);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	assert(svin_input_ready_pending(6));
	screen_set(&screen_load_world);
	waiting_marker.payload.player_pos.input_epoch = 7;
	clin_process(&waiting_marker);
	ptime_t newer_clock = gstate.world_time_start;
	struct input_gate_capture older_queued_ack = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &older_queued_ack, false);
	svin_rpc_flush_pending();
	svin_process_messages(capture_input_gate_transport, &older_queued_ack, false);
	assert(older_queued_ack.count == 17 && older_queued_ack.ack_epoch == 6);
	clin_update();
	assert(gstate.world_switch_pending
		   && gstate.players[0].current_screen == &screen_load_world);
	assert(memcmp(&gstate.world_time_start, &newer_clock, sizeof(newer_clock)) == 0);
	screen_load_world.update(&screen_load_world, 30.0f);
	clin_update_input_readiness();
	struct input_gate_capture newer_ack = {.ack_index = SIZE_MAX};
	svin_process_messages(capture_input_gate_transport, &newer_ack, false);
	assert(newer_ack.count == 1 && newer_ack.ack_epoch == 7
		   && !newer_ack.retry && !gstate.world_switch_pending);
	puts("R1 PASS: newer installed marker retires the previous ACK wait");

	/* The remainder of this executable creates independent in-process server
	 * fixtures whose counters restart. Model their process boundary explicitly. */
	clin_test_reset_input_epoch_guard();
	puts("  portal: live voxel OOM recovery preserves pause/window ownership");
	puts("  portal: queued input ACK gates backlog; generic RETURN preserves pause");

	/* Restore the deliberately reset-unready baseline expected by the following
	 * transactional-window fixture, while releasing every client allocation. */
	clin_set_alloc_hook(fail_client_alloc);
	clin_process(&reset);
	clin_set_alloc_hook(NULL);
	world_destroy(&gstate.world);
	dict_entity_clear(gstate.entities);
	memset(&gstate, 0, sizeof(gstate));
}

static int test_container_close_count;
static bool test_container_fail_next_close;

static bool test_container_destroy(struct inventory* inv,
								   struct server_player* player) {
	(void)inv;
	(void)player;
	return true;
}

static bool test_container_close(struct inventory* inv,
								 struct server_player* player) {
	test_container_close_count++;
	if(test_container_fail_next_close) {
		test_container_fail_next_close = false;
		return false;
	}
	inventory_destroy_owned(inv, player);
	return true;
}

static struct inventory_logic test_container_logic = {
	.on_destroy = test_container_destroy,
	.on_close = test_container_close,
};

static void deliver_after_full_server_inbox(struct server_local* server,
										 const struct server_rpc* delayed) {
	assert(server && delayed && svin_pending_count() == 0);
	// Fill the 16-token live inbox first. The target action must therefore enter
	// the reliable pending ring, exactly like a GUI action delayed by a busy
	// server during portal teardown.
	for(int i = 0; i < 16; i++) {
		struct server_rpc filler = {.type = SRPC_PLAYER_POS};
		svin_rpc_send(&filler);
	}
	assert(svin_pending_count() == 0);
	struct server_rpc action = *delayed;
	svin_rpc_send(&action);
	assert(svin_pending_count() == 1);

	server_local_test_process_messages(server); // server frees the live inbox
	assert(svin_pending_count() == 1);
	svin_rpc_flush_pending(); // next clin_update retries the old GUI action
	assert(svin_pending_count() == 0);
	server_local_test_process_messages(server);
}

static void test_stale_window_rpcs_do_not_cross_world_load(void) {
	struct server_local server = {0};
	assert(inventory_create(&server.player.inventory, &inventory_logic_player,
							&server, &server.player, INVENTORY_SIZE, 0, 0, 0));
	server.player.active_inventory = &server.player.inventory;
	server.player.has_pos = true;
	server.player.finished_loading = false;
	dict_entity_init(server.entities);

	inventory_set_slot(&server.player.inventory, INVENTORY_SLOT_HOTBAR,
		(struct item_data) {.id = BLOCK_STONE, .count = 4});
	inventory_set_slot(&server.player.inventory, INVENTORY_SLOT_CRAFTING,
		(struct item_data) {.id = ITEM_APPLE, .count = 2});
	inventory_set_picked_item(&server.player.inventory,
		(struct item_data) {.id = ITEM_BREAD, .count = 3});
	struct item_data expected[INVENTORY_SIZE];
	memcpy(expected, server.player.inventory.items, sizeof(expected));

	const struct server_rpc loading_click = {
		.type = SRPC_WINDOW_CLICK,
		.payload.window_click = {.window = WINDOWC_INVENTORY,
			.slot = INVENTORY_SLOT_HOTBAR, .action_id = 41},
	};
	deliver_after_full_server_inbox(&server, &loading_click);
	const struct server_rpc loading_close = {
		.type = SRPC_WINDOW_CLOSE,
		.payload.window_close.window = WINDOWC_INVENTORY,
	};
	deliver_after_full_server_inbox(&server, &loading_close);
	const struct server_rpc loading_drop = {
		.type = SRPC_WINDOW_DROP,
		.payload.window_drop = {.window = WINDOWC_INVENTORY, .right = false},
	};
	deliver_after_full_server_inbox(&server, &loading_drop);

	assert_inventory_items_equal(&server.player.inventory, expected);
	struct item_data picked;
	assert(inventory_get_picked_item(&server.player.inventory, &picked));
	assert(picked.id == ITEM_BREAD && picked.count == 3);
	assert(count_item_entities(&server, ITEM_APPLE, 2) == 0);
	assert(count_item_entities(&server, ITEM_BREAD, 3) == 0);

	// Even after loading, a delayed message for a closed chest window must not
	// be interpreted against the 45-slot player inventory merely because its
	// slot number happens to fit.
	server.player.finished_loading = true;
	/* Deviation-#18: zero-init health reads as DEAD; be explicitly alive. */
	server.player.health = MAX_PLAYER_HEALTH;
	struct server_rpc wrong_window = loading_click;
	wrong_window.payload.window_click.window = WINDOWC_CHEST;
	wrong_window.payload.window_click.action_id = 42;
	deliver_after_full_server_inbox(&server, &wrong_window);
	assert_inventory_items_equal(&server.player.inventory, expected);
	assert(inventory_get_picked_item(&server.player.inventory, &picked));
	assert(picked.id == ITEM_BREAD && picked.count == 3);

	// A valid loaded-window cursor drop is also allocate-before-mutate. If the
	// EntityItem allocation fails, the authoritative cursor remains byte-exact.
	server_local_test_fail_next_entity_alloc();
	deliver_after_full_server_inbox(&server, &loading_drop);
	assert(inventory_get_picked_item(&server.player.inventory, &picked));
	assert(picked.id == ITEM_BREAD && picked.count == 3);
	assert(count_item_entities(&server, ITEM_BREAD, 3) == 0);

	// If a prior unload was cancelled, a fast world-select click can enqueue a
	// LOAD before the cancellation RPC is rendered. The live-world has_pos gate
	// must reject it instead of constructing a second world over the first.
	struct server_rpc premature_load = {.type = SRPC_LOAD_WORLD};
	string_init_set_str(premature_load.payload.load_world.name,
						"must_not_load_over_live_world");
	svin_rpc_send(&premature_load);
	server_local_test_process_messages(&server);
	assert(server.player.has_pos
		   && server.player.active_inventory == &server.player.inventory);

	inventory_destroy(&server.player.inventory, &server.player);
	dict_entity_clear(server.entities);
	puts("  portal: delayed old-window RPC backlog cannot mutate carried inventory");
}

static void* fail_client_alloc(size_t size) {
	(void)size;
	return NULL;
}
static void* poison_client_alloc(size_t size) {
	void* ptr = malloc(size);
	if(ptr)
		memset(ptr, 0xBE, size);
	return ptr;
}
static int client_allocs_until_failure;
static void* fail_nth_client_alloc(size_t size) {
	if(--client_allocs_until_failure == 0)
		return NULL;
	return malloc(size);
}

#include "support/shared_chest_cases.h"

static void test_transactional_window_open(void) {
	struct server_local server = {0};
	string_t window_level;
	string_init_set_str(window_level, TMP_WORLD);
	server_world_create(&server.world, window_level, WORLD_DIM_OVERWORLD, 31337,
						true);
	server.worlds[0].session = &server; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&server))
		abort();
	struct server_chunk* window_chunk = NULL;
	assert(server_world_load_chunk(&server.world, 0, 0, &window_chunk)
		   && window_chunk);
	assert(inventory_create(&server.player.inventory, &inventory_logic_player,
								&server, &server.player, INVENTORY_SIZE, 0, 0, 0));
	server.player.active_inventory = &server.player.inventory;
	server.player.has_pos = true;
	server.player.finished_loading = true;
	server.player.health = MAX_PLAYER_HEALTH;
	server.player.x = -100.0;
	server.player.y = 70.0;
	server.player.z = -100.0;
	dict_entity_init(server.entities);
	inventory_set_slot(
		&server.player.inventory, INVENTORY_SLOT_HOTBAR,
		(struct item_data) {.id = ITEM_APPLE, .durability = 4, .count = 5});

	// Neither layer of a server-side allocation may publish OPEN or replace the
	// stable player owner. The same holds when a storage window has no durable
	// tile-entity row to receive its contents on close.
	server_local_test_fail_next_window_alloc();
	assert(!server_local_open_inventory(
		&server, &inventory_logic_crafting, CRAFTING_SIZE, 0, 0, 0,
		WINDOWC_CRAFTING, WINDOW_TYPE_WORKBENCH));
	assert(server.player.active_inventory == &server.player.inventory);
	inventory_test_set_alloc_hook(fail_client_alloc);
	assert(!server_local_open_inventory(
		&server, &inventory_logic_crafting, CRAFTING_SIZE, 0, 0, 0,
		WINDOWC_CRAFTING, WINDOW_TYPE_WORKBENCH));
	inventory_test_set_alloc_hook(NULL);
	assert(server.player.active_inventory == &server.player.inventory);
	assert(!server_local_open_inventory(
		&server, &inventory_logic_chest, CHEST_SIZE, 101, 70, 101,
		WINDOWC_CHEST, WINDOW_TYPE_CHEST));
	assert(!server_local_open_linked_inventory(
		&server, &inventory_logic_double_chest, IRON_CHEST_SIZE, 101, 70,
		101, 102, 70, 101, WINDOWC_IRON_CHEST, WINDOW_TYPE_IRON_CHEST));
	assert(!server_local_open_inventory(
		&server, &inventory_logic_iron_chest, IRON_CHEST_SIZE, 101, 70, 101,
		WINDOWC_IRON_CHEST, WINDOW_TYPE_IRON_CHEST));
	assert(!server_local_open_inventory(
		&server, &inventory_logic_furnace, FURNACE_SIZE, 101, 70, 101,
		WINDOWC_FURNACE, WINDOW_TYPE_FURNACE));
	assert(!server_local_open_inventory(
		&server, &inventory_logic_sign, SIGN_SIZE, 101, 70, 101,
		WINDOWC_SIGN, WINDOW_TYPE_SIGN));
	assert(!server_local_open_inventory(
		&server, &inventory_logic_crafting, CHEST_SIZE, 0, 0, 0,
		WINDOWC_CRAFTING, WINDOW_TYPE_WORKBENCH));
	assert(server.player.active_inventory == &server.player.inventory);

	// Successful creation is populated privately, then published. Because the
	// client is deliberately reset-unready here, its explicit CLOSE rejection
	// must reach and tear down the real authoritative window.
	assert(server_local_open_inventory(
		&server, &inventory_logic_crafting, CRAFTING_SIZE, 0, 0, 0,
		WINDOWC_CRAFTING, WINDOW_TYPE_WORKBENCH));
	struct inventory* opened = server.player.active_inventory;
	assert(opened != &server.player.inventory);
	struct item_data observed;
	assert(inventory_get_slot(opened, CRAFTING_SLOT_HOTBAR, &observed));
	assert(observed.id == ITEM_APPLE && observed.durability == 4
		   && observed.count == 5);
	assert(!server_local_open_inventory(
		&server, &inventory_logic_crafting, CRAFTING_SIZE, 0, 0, 0,
		WINDOWC_CRAFTING, WINDOW_TYPE_WORKBENCH));
	assert(server.player.active_inventory == opened);
	struct client_rpc open = {
		.type = CRPC_OPEN_WINDOW,
		.payload.window_open = {
			.window = WINDOWC_CRAFTING,
			.type = WINDOW_TYPE_WORKBENCH,
			.slot_count = CRAFTING_SIZE,
		},
	};
	// Saturate both the 16-token inbox and 64-entry ordinary-action budget with
	// stale clicks. The reset-unready client's CLOSE rejection is lifecycle-
	// critical and must occupy reserved FIFO capacity after every click.
	for(int i = 0; i < 80; i++) {
		struct server_rpc filler = {
			.type = SRPC_WINDOW_CLICK,
			.payload.window_click = {
				.window = WINDOWC_CHEST,
				.slot = 0,
				.action_id = (uint16_t)i,
			},
		};
		svin_rpc_send(&filler);
	}
	assert(svin_pending_count() == 64);
	clin_process(&open);
	assert(svin_pending_count() == 65);
	while(svin_pending_count() > 0) {
		server_local_test_process_messages(&server);
		svin_rpc_flush_pending();
	}
	server_local_test_process_messages(&server);
	assert(server.player.active_inventory == &server.player.inventory);

	// Window 0 has the same fallible close contract as block containers. A
	// failed crafting/cursor drop must keep its authoritative state and trigger
	// the self-contained player REOPEN path; a later retry commits once.
	inventory_set_slot(
		&server.player.inventory, INVENTORY_SLOT_CRAFTING,
		(struct item_data) {.id = ITEM_BREAD, .durability = 3, .count = 2});
	inventory_set_picked_item(
		&server.player.inventory,
		(struct item_data) {.id = ITEM_COAL, .durability = 1, .count = 3});
	inventory_set_hotbar(&server.player.inventory, 7);
	struct server_rpc close_player = {
		.type = SRPC_WINDOW_CLOSE,
		.payload.window_close.window = WINDOWC_INVENTORY,
	};
	server_local_test_fail_next_entity_alloc();
	server_local_test_process_rpc(&server, &close_player);
	assert(server.player.active_inventory == &server.player.inventory);
	assert(inventory_get_slot(&server.player.inventory,
							  INVENTORY_SLOT_CRAFTING, &observed));
	assert(observed.id == ITEM_BREAD && observed.durability == 3
		   && observed.count == 2);
	assert(inventory_get_picked_item(&server.player.inventory, &observed));
	assert(observed.id == ITEM_COAL && observed.durability == 1
		   && observed.count == 3);
	assert(inventory_get_hotbar(&server.player.inventory) == 7);
	server_local_test_process_rpc(&server, &close_player);
	assert(!inventory_get_slot(&server.player.inventory,
							   INVENTORY_SLOT_CRAFTING, NULL));
	assert(!inventory_get_picked_item(&server.player.inventory, NULL));
	assert(inventory_get_hotbar(&server.player.inventory) == 7);

	// The stable player's transient close must precede the block container. A
	// failed cursor drop leaves that container live; retry commits each owner
	// exactly once.
	struct inventory* container = calloc(1, sizeof(*container));
	assert(container && inventory_create(container, &test_container_logic, NULL,
										 &server.player, 1, 0, 0, 0));
	server.player.active_inventory = container;
	inventory_set_picked_item(
		&server.player.inventory,
		(struct item_data) {.id = ITEM_BREAD, .durability = 8, .count = 1});
	test_container_close_count = 0;
	server_local_test_fail_next_entity_alloc();
	assert(!server_local_test_close_active_inventory(&server));
	assert(server.player.active_inventory == container);
	assert(test_container_close_count == 0);
	assert(inventory_get_picked_item(&server.player.inventory, &observed));
	assert(observed.id == ITEM_BREAD && observed.durability == 8
		   && observed.count == 1);
	assert(server_local_test_close_active_inventory(&server));
	assert(server.player.active_inventory == &server.player.inventory);
	assert(test_container_close_count == 1);
	assert(!inventory_get_picked_item(&server.player.inventory, NULL));
	assert(count_item_entities(&server, ITEM_BREAD, 1) == 1);

	// The inverse partial failure is also idempotent: player transient close
	// succeeds, then the container refuses once. Retry must not spawn the first
	// owner's cursor a second time.
	container = calloc(1, sizeof(*container));
	assert(container && inventory_create(container, &test_container_logic, NULL,
										 &server.player, 1, 0, 0, 0));
	server.player.active_inventory = container;
	inventory_set_picked_item(
		&server.player.inventory,
		(struct item_data) {.id = ITEM_DIAMOND, .durability = 6, .count = 1});
	int diamonds_before = count_item_entities(&server, ITEM_DIAMOND, 1);
	test_container_close_count = 0;
	test_container_fail_next_close = true;
	assert(!server_local_test_close_active_inventory(&server));
	assert(server.player.active_inventory == container);
	assert(test_container_close_count == 1);
	assert(!inventory_get_picked_item(&server.player.inventory, NULL));
	assert(count_item_entities(&server, ITEM_DIAMOND, 1)
		   == diamonds_before + 1);
	assert(server_local_test_close_active_inventory(&server));
	assert(test_container_close_count == 2);
	assert(count_item_entities(&server, ITEM_DIAMOND, 1)
		   == diamonds_before + 1);

	// Exact TNT ownership regression: container teardown runs regardless of the
	// 33% block-item drop roll. The authoritative open-window storage spills at
	// onBlockRemoval time, clears immediately, and close cannot duplicate it.
	const w_coord_t chest_x = 4, chest_y = 70, chest_z = 4;
	assert(server_world_set_block(
		&server.worlds[0], chest_x, chest_y, chest_z,
		(struct block_data) {.type = BLOCK_CHEST}));
	server.chest_pos[0]
		= (struct complex_block_pos) {chest_x, chest_y, chest_z};
	/* A1b: the fixture row names its world (no unstamped-row fallback). */
	server.te_owners.chest_slot[0] = server.worlds[0].world_index;
	server.te_owners.chest_gen[0] = server.worlds[0].generation;
	server.chest_items[0][0]
		= (struct item_data) {.id = ITEM_APPLE, .durability = 2, .count = 2};
	assert(server_local_open_inventory(
		&server, &inventory_logic_chest, CHEST_SIZE, chest_x, chest_y,
		chest_z, WINDOWC_CHEST, WINDOW_TYPE_CHEST));
	struct inventory* chest_window = server.player.active_inventory;
	assert(chest_window != &server.player.inventory);
	server_world_begin_update(&server.world);
	server_world_explode(
		&server.worlds[0], (vec3) {chest_x + 0.5F, chest_y + 0.5F, chest_z + 0.5F},
		4.0F);
	struct block_data removed;
	assert(server_world_get_block(&server.world, chest_x, chest_y, chest_z,
								  &removed));
	assert(removed.type == BLOCK_AIR && server.chest_pos[0].y < 0);
	assert(server.player.active_inventory == chest_window);
	assert(count_item_entities(&server, ITEM_APPLE, 2) == 1);
	assert(!inventory_get_slot(chest_window, CHEST_SLOT_STORAGE, NULL));
	assert(server_local_test_close_active_inventory(&server));
	assert(count_item_entities(&server, ITEM_APPLE, 2) == 1);

	// If the immediate storage batch cannot allocate, onBreak still invalidates
	// the removed backing owner but leaves every live window slot intact. The
	// ordinary close path is then the one documented OOM-only delayed spill.
	const w_coord_t oom_chest_x = 7, oom_chest_y = 70, oom_chest_z = 7;
	assert(server_world_set_block(
		&server.worlds[0], oom_chest_x, oom_chest_y, oom_chest_z,
		(struct block_data) {.type = BLOCK_CHEST}));
	server.chest_pos[0]
		= (struct complex_block_pos) {oom_chest_x, oom_chest_y, oom_chest_z};
	server.te_owners.chest_slot[0] = server.worlds[0].world_index; /* A1b */
	server.te_owners.chest_gen[0] = server.worlds[0].generation;
	server.chest_items[0][0]
		= (struct item_data) {.id = ITEM_BREAD, .durability = 5, .count = 4};
	assert(server_local_open_inventory(
		&server, &inventory_logic_chest, CHEST_SIZE, oom_chest_x,
		oom_chest_y, oom_chest_z, WINDOWC_CHEST, WINDOW_TYPE_CHEST));
	struct inventory* oom_chest_window = server.player.active_inventory;
	server_local_test_fail_next_container_drop();
	server_world_begin_update(&server.world);
	server_world_explode(
		&server.worlds[0],
		(vec3) {oom_chest_x + 0.5F, oom_chest_y + 0.5F,
					oom_chest_z + 0.5F},
		4.0F);
	assert(server_world_get_block(&server.world, oom_chest_x, oom_chest_y,
								  oom_chest_z, &removed));
	assert(removed.type == BLOCK_AIR && server.chest_pos[0].y < 0);
	assert(inventory_get_slot(
		oom_chest_window, CHEST_SLOT_STORAGE, &observed));
	assert(observed.id == ITEM_BREAD && observed.durability == 5
		   && observed.count == 4);
	assert(count_item_entities(&server, ITEM_BREAD, 4) == 0);
	assert(server_local_test_close_active_inventory(&server));
	assert(count_item_entities(&server, ITEM_BREAD, 4) == 1);

	const w_coord_t furnace_x = 10, furnace_y = 70, furnace_z = 10;
	assert(server_world_set_block(
		&server.worlds[0], furnace_x, furnace_y, furnace_z,
		(struct block_data) {.type = BLOCK_FURNACE}));
	assert(furnace_te_register(&server, furnace_x, furnace_y, furnace_z) >= 0);
	struct furnace_te* furnace
		= furnace_te_lookup(&server, furnace_x, furnace_y, furnace_z);
	assert(furnace);
	furnace->items[FURNACE_SLOT_INPUT_TE]
		= (struct item_data) {.id = BLOCK_IRON_ORE, .durability = 0, .count = 2};
	/* A load/demo window is armed before the final marker but must not publish
	 * until the exact input epoch reaches the server. Wrong epochs retain both
	 * the input barrier and the deferred authoritative container. */
	assert(!server.furnace_demo_open_pending);
	server.furnace_demo_open_pending = true;
	server.furnace_demo_open_x = furnace_x;
	server.furnace_demo_open_y = furnace_y;
	server.furnace_demo_open_z = furnace_z;
	server.input_ack_pending = 701;
	struct server_rpc wrong_demo_ack = {
		.type = SRPC_INPUT_READY,
		.payload.input_ready = {.epoch = 702, .retry = false},
	};
	server_local_test_process_rpc(&server, &wrong_demo_ack);
	assert(server.input_ack_pending == 701);
	assert(server.furnace_demo_open_pending);
	assert(server.player.active_inventory == &server.player.inventory);
	struct server_rpc demo_ack = {
		.type = SRPC_INPUT_READY,
		.payload.input_ready = {.epoch = 701, .retry = false},
	};
	server_local_test_process_rpc(&server, &demo_ack);
	assert(server.input_ack_pending == 0);
	assert(!server.furnace_demo_open_pending);
	assert(server.player.active_inventory != &server.player.inventory);
	assert(server.player.active_inventory->logic == &inventory_logic_furnace);
	assert(server_local_test_close_active_inventory(&server));
	assert(server_local_open_inventory(
		&server, &inventory_logic_furnace, FURNACE_SIZE, furnace_x,
		furnace_y, furnace_z, WINDOWC_FURNACE, WINDOW_TYPE_FURNACE));
	struct inventory* furnace_window = server.player.active_inventory;
	server_world_begin_update(&server.world);
	server_world_explode(
		&server.worlds[0],
		(vec3) {furnace_x + 0.5F, furnace_y + 0.5F, furnace_z + 0.5F},
		4.0F);
	assert(server_world_get_block(&server.world, furnace_x, furnace_y,
								  furnace_z, &removed));
	assert(removed.type == BLOCK_AIR
		   && !furnace_te_lookup(&server, furnace_x, furnace_y, furnace_z));
	assert(server.player.active_inventory == furnace_window);
	assert(count_item_entities(&server, BLOCK_IRON_ORE, 2) == 1);
	assert(!inventory_get_slot(furnace_window, FURNACE_SLOT_INPUT, NULL));
	assert(server_local_test_close_active_inventory(&server));
	assert(count_item_entities(&server, BLOCK_IRON_ORE, 2) == 1);

	// A closed furnace whose content spill sustains two allocation failures
	// retains its TE as a no-loss owner, but that orphan must be completely
	// frozen while the world cell is air. The third retry spills the exact
	// original stacks once and releases the TE.
	const w_coord_t orphan_fx = 13, orphan_fy = 70, orphan_fz = 13;
	assert(server_world_set_block(
		&server.worlds[0], orphan_fx, orphan_fy, orphan_fz,
		(struct block_data) {.type = BLOCK_FURNACE}));
	assert(furnace_te_register(&server, orphan_fx, orphan_fy, orphan_fz) >= 0);
	struct furnace_te* orphan_f
		= furnace_te_lookup(&server, orphan_fx, orphan_fy, orphan_fz);
	assert(orphan_f);
	orphan_f->burn_time = 40;
	orphan_f->cook_time = 73;
	orphan_f->current_item_burn_time = 1600;
	orphan_f->items[FURNACE_SLOT_INPUT_TE]
		= (struct item_data) {.id = BLOCK_IRON_ORE, .count = 3};
	orphan_f->items[FURNACE_SLOT_FUEL_TE]
		= (struct item_data) {.id = ITEM_COAL, .count = 1};
	orphan_f->items[FURNACE_SLOT_OUTPUT_TE]
		= (struct item_data) {.id = ITEM_IRON, .count = 1};
	int orphan_ore_before
		= count_item_entities(&server, BLOCK_IRON_ORE, 3);
	int orphan_coal_before = count_item_entities(&server, ITEM_COAL, 1);
	int orphan_iron_before
		= count_item_entities(&server, ITEM_IRON, 1);
	server_local_test_fail_next_container_drop();
	server_world_begin_update(&server.world);
	server_world_explode(
		&server.worlds[0],
		(vec3) {orphan_fx + 0.5F, orphan_fy + 0.5F, orphan_fz + 0.5F},
		4.0F);
	orphan_f = furnace_te_lookup(&server, orphan_fx, orphan_fy, orphan_fz);
	assert(orphan_f && orphan_f->burn_time == 40 && orphan_f->cook_time == 73
		   && orphan_f->current_item_burn_time == 1600);
	server_local_test_fail_next_container_drop();
	server_local_test_retry_orphaned_container_drops(&server);
	orphan_f = furnace_te_lookup(&server, orphan_fx, orphan_fy, orphan_fz);
	assert(orphan_f);
	furnace_server_tick(&server);
	assert(orphan_f->burn_time == 40 && orphan_f->cook_time == 73
		   && orphan_f->current_item_burn_time == 1600);
	assert(orphan_f->items[FURNACE_SLOT_INPUT_TE].id == BLOCK_IRON_ORE
		   && orphan_f->items[FURNACE_SLOT_INPUT_TE].count == 3);
	assert(orphan_f->items[FURNACE_SLOT_FUEL_TE].id == ITEM_COAL
		   && orphan_f->items[FURNACE_SLOT_FUEL_TE].count == 1);
	assert(orphan_f->items[FURNACE_SLOT_OUTPUT_TE].id == ITEM_IRON
		   && orphan_f->items[FURNACE_SLOT_OUTPUT_TE].count == 1);
	server_local_test_retry_orphaned_container_drops(&server);
	assert(!furnace_te_lookup(&server, orphan_fx, orphan_fy, orphan_fz));
	assert(count_item_entities(&server, BLOCK_IRON_ORE, 3)
		   == orphan_ore_before + 1);
	assert(count_item_entities(&server, ITEM_COAL, 1)
		   == orphan_coal_before + 1);
	assert(count_item_entities(&server, ITEM_IRON, 1)
		   == orphan_iron_before + 1);

	// Closed-container contents use BlockChest's private cosmetic RNG stream.
	// Two translated, otherwise identical explosions must leave the world RNG
	// at the same state whether the removed chest was full or empty.
	for(int cx = 1; cx <= 4; cx++)
		assert(server_world_load_chunk(&server.world, cx, 0, &window_chunk)
			   && window_chunk);
	const w_coord_t rng_full_x = 20, rng_empty_x = 52, rng_y = 70, rng_z = 4;
	assert(server_world_set_block(
		&server.worlds[0], rng_full_x, rng_y, rng_z,
		(struct block_data) {.type = BLOCK_CHEST}));
	assert(server_world_set_block(
		&server.worlds[0], rng_empty_x, rng_y, rng_z,
		(struct block_data) {.type = BLOCK_CHEST}));
	server.chest_pos[0]
		= (struct complex_block_pos) {rng_full_x, rng_y, rng_z};
	server.te_owners.chest_slot[0] = server.worlds[0].world_index; /* A1b */
	server.te_owners.chest_gen[0] = server.worlds[0].generation;
	server.chest_pos[1]
		= (struct complex_block_pos) {rng_empty_x, rng_y, rng_z};
	server.te_owners.chest_slot[1] = server.worlds[0].world_index; /* A1b */
	server.te_owners.chest_gen[1] = server.worlds[0].generation;
	memset(server.chest_items[0], 0, sizeof(server.chest_items[0]));
	memset(server.chest_items[1], 0, sizeof(server.chest_items[1]));
	server.chest_items[0][0]
		= (struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 6};
	const uint32_t explosion_seed = 0x12345678u;
	server.rand_src.seed = explosion_seed;
	/* F4-RNG: the blast's rays/sound/drops draw World.rand; the container
	 * spill must not perturb it either. */
	jrand_set_seed(&server.worlds[0].rand, (int64_t)explosion_seed);
	server_world_begin_update(&server.world);
	server_world_explode(
		&server.worlds[0], (vec3) {rng_full_x + 0.5F, rng_y + 0.5F, rng_z + 0.5F},
		4.0F);
	uint32_t filled_world_rng_after = server.rand_src.seed;
	const uint64_t filled_world_rand_after = server.worlds[0].rand.seed;
	assert(count_item_entities(&server, ITEM_STICK, 6) == 1);
	server.rand_src.seed = explosion_seed;
	jrand_set_seed(&server.worlds[0].rand, (int64_t)explosion_seed);
	server_world_begin_update(&server.world);
	server_world_explode(
		&server.worlds[0], (vec3) {rng_empty_x + 0.5F, rng_y + 0.5F, rng_z + 0.5F},
		4.0F);
	assert(server.rand_src.seed == filled_world_rng_after);
	assert(server.worlds[0].rand.seed == filled_world_rand_after);

	// Exact Beta chance contract: <= 0.3F per quantityDropped unit, no outer
	// roll for zero-quantity blocks, and the crop override always runs its seed
	// trials even when the wheat chance fails. Each accepted unit then consumes
	// exactly three World.rand position offsets (x/y/z) before EntityItem uses
	// its independent Math.random constructor stream.
	struct block_data chance_block = {.type = BLOCK_STONE};
	struct block_info chance_info = {
		.block = &chance_block, .x = 40, .y = 75, .z = 4,
	};
	int stone_units_before
		= count_item_entities(&server, BLOCK_COBBLESTONE, 1);
	/* F4-RNG (ASTRA-RNG2-RULING 1/2/6): every drop draws the affected world's
	 * java.util.Random World.rand. Vectors re-derived on the JVM by
	 * tmp/jobs/lead-1010/f4rng-witness/F4PortalDropWitness.java (OpenJDK
	 * 21.0.7; source sha256 c52f7e7c..., output sha256 fb34f83a...), which
	 * replays the decompiled Block/BlockCrops/BlockGravel/BlockTallGrass/
	 * BlockGlowStone drop methods; the xorshift-era seeds/states were pinned
	 * from the candidate itself and are superseded. The session xorshift
	 * must not move. */
	struct java_random* world_rand = &server.worlds[0].rand;
	const struct random_gen session_before = server.rand_src;
	jrand_set_seed(world_rand, INT64_C(22092090)); // first nextFloat == 0.3F
	struct java_random threshold_probe = *world_rand;
	assert(jrand_next_float(&threshold_probe) == 0.3F);
	server_local_spawn_block_drops_with_chance(&server.worlds[0], &chance_info, 0.3F);
	assert(count_item_entities(&server, BLOCK_COBBLESTONE, 1)
		   == stone_units_before + 1); // pins <=, not <
	assert(world_rand->seed == UINT64_C(0x84da25180acb)); // chance + 3 offsets

	chance_block.type = BLOCK_CLAY;
	int clay_units_before = count_item_entities(&server, ITEM_CLAY_BALL, 1);
	jrand_set_seed(world_rand, INT64_C(1));
	server_local_spawn_block_drops_with_chance(&server.worlds[0], &chance_info, 0.3F);
	assert(count_item_entities(&server, ITEM_CLAY_BALL, 1)
		   == clay_units_before + 2); // four independent rolls, not all-or-none
	assert(world_rand->seed == UINT64_C(0xb5f30868620e)); // 4 chance + 2*3 offsets

	chance_block.type = BLOCK_GLASS;
	jrand_set_seed(world_rand, INT64_C(1));
	server_local_spawn_block_drops_with_chance(&server.worlds[0], &chance_info, 0.3F);
	assert(world_rand->seed == UINT64_C(0x5deece66c)); // quantityDropped==0: no chance draw

	chance_block.type = BLOCK_CROPS;
	chance_block.metadata = 0;
	int seeds_before = count_item_entities(&server, ITEM_SEED, 1);
	int wheat_before = count_item_entities(&server, ITEM_WHEAT, 1);
	jrand_set_seed(world_rand, INT64_C(4125)); // chance passes, meta 0 suppresses wheat; only the first seed trial passes
	server_local_spawn_block_drops_with_chance(&server.worlds[0], &chance_info, 0.3F);
	assert(count_item_entities(&server, ITEM_WHEAT, 1) == wheat_before);
	assert(count_item_entities(&server, ITEM_SEED, 1) == seeds_before + 1);
	assert(world_rand->seed == UINT64_C(0xfc0c10defd41)); // wheat chance + trial + 3 float offsets + 2 trials

	// idDropped(meta, rand) overrides roll only after chance passes. A failed
	// gravel chance consumes exactly one draw; passing vectors pin the later
	// flint/seed selection followed by exactly three position draws.
	chance_block.type = BLOCK_GRAVEL;
	int gravel_before = count_item_entities(&server, BLOCK_GRAVEL, 1);
	int flint_before = count_item_entities(&server, ITEM_FLINT, 1);
	jrand_set_seed(world_rand, INT64_C(1)); // first chance draw is > 0.3
	server_local_spawn_block_drops_with_chance(&server.worlds[0], &chance_info, 0.3F);
	assert(count_item_entities(&server, BLOCK_GRAVEL, 1) == gravel_before);
	assert(count_item_entities(&server, ITEM_FLINT, 1) == flint_before);
	assert(world_rand->seed == UINT64_C(0xbb1ad5732407)); // no premature idDropped draw
	jrand_set_seed(world_rand, INT64_C(4129)); // chance passes; nextInt(10)==0 selects flint
	server_local_spawn_block_drops_with_chance(&server.worlds[0], &chance_info, 0.3F);
	assert(count_item_entities(&server, ITEM_FLINT, 1) == flint_before + 1);
	assert(world_rand->seed == UINT64_C(0x84dcc9c892db));

	chance_block.type = BLOCK_TALL_GRASS;
	seeds_before = count_item_entities(&server, ITEM_SEED, 1);
	jrand_set_seed(world_rand, INT64_C(4104)); // chance passes; nextInt(8)==0 selects seed
	server_local_spawn_block_drops_with_chance(&server.worlds[0], &chance_info, 0.3F);
	assert(count_item_entities(&server, ITEM_SEED, 1) == seeds_before + 1);
	assert(world_rand->seed == UINT64_C(0x42f57e2c58d0));

	// Generic random quantity is drawn first, then chance is rolled per unit.
	// This seed yields glowstone quantity 4 and exactly two accepted dust units;
	// their six position draws are interleaved before the later chance rolls.
	chance_block.type = BLOCK_GLOWSTONE;
	int dust_before
		= count_item_entities(&server, ITEM_GLOWSTONE_DUST, 1);
	jrand_set_seed(world_rand, INT64_C(19));
	server_local_spawn_block_drops_with_chance(&server.worlds[0], &chance_info, 0.3F);
	assert(count_item_entities(&server, ITEM_GLOWSTONE_DUST, 1)
		   == dust_before + 2);
	assert(world_rand->seed == UINT64_C(0xe4d9307045fb));
	assert(server.rand_src.seed == session_before.seed); // never the session stream

	// Death closes the player 2x2 container before dropping owned inventory.
	// Slot 0 is a virtual recipe result: inputs drop, but the uncrafted output
	// must not also materialize.
	server.player.active_inventory = &server.player.inventory;
	server.player.health = MAX_PLAYER_HEALTH;
	inventory_set_slot(
		&server.player.inventory, INVENTORY_SLOT_CRAFTING,
		(struct item_data) {.id = BLOCK_LOG, .count = 1});
	inventory_set_slot(
		&server.player.inventory, INVENTORY_SLOT_OUTPUT,
		(struct item_data) {.id = BLOCK_PLANKS, .count = 4});
	int logs_before = count_item_entities(&server, BLOCK_LOG, 1);
	int planks_before = count_item_entities(&server, BLOCK_PLANKS, 4);
	assert(server_local_set_player_health(&server, 0));
	/* Deviation-#18 split: the close + drops run in the respawn transaction. */
	assert(server_local_respawn_request(&server));
	assert(count_item_entities(&server, BLOCK_LOG, 1) == logs_before + 1);
	assert(count_item_entities(&server, BLOCK_PLANKS, 4) == planks_before);
	assert(!inventory_get_slot(
		&server.player.inventory, INVENTORY_SLOT_OUTPUT, NULL));
	assert(!inventory_get_slot(
		&server.player.inventory, INVENTORY_SLOT_CRAFTING, NULL));

	// The respawn creates a fresh player before the tick's later portal
	// state-machine half. A threshold-ready Nether portal latch must not beat
	// the deferred Overworld death eviction (deviation-#18: the corpse keeps
	// its latched transients through the dwell -- the alive-gated travel
	// consumer never fires -- and the respawn's fresh-entity resets clear
	// them, exactly like Beta's fresh EntityPlayer).
	server.world.dimension = WORLD_DIM_NETHER;
	server.player.dimension = WORLD_DIM_NETHER;
	/* The previous sub-test's respawn left its loading epoch armed. */
	server.player.finished_loading = true;
	server.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
	server.player.health = 1;
	server.player.time_until_portal = 0;
	server.player.time_in_portal = 0.99F;
	server.player.in_portal = true;
	server.player.portal_travel_requested = true;
	assert(server_local_set_player_health(&server, 0));
	assert(server.player.portal_travel_requested); /* corpse keeps the latch */
	assert(!server.death_world_evict);
	assert(server_local_respawn_request(&server));
	server_player_portal_tick(&server.player);
	assert(server.death_world_evict && server.death_world_retry_delay == 0);
	assert(server.player.dimension == WORLD_DIM_OVERWORLD);
	assert(!server.player.portal_travel_requested && !server.player.in_portal);
	assert(server.player.time_in_portal == 0.0F
		   && server.player.time_until_portal == 19);

	// The checked death transaction can back off with an Overworld respawn
	// player still paired to the live Nether source. Every queued stateful RPC,
	// including ordinary UNLOAD, is barred until that transaction succeeds.
	server.death_world_retry_delay = 20;
	server.player.finished_loading = true;
	/* Deviation-#18: zero-init health reads as DEAD; be explicitly alive. */
	server.player.health = MAX_PLAYER_HEALTH;
	server.player.active_inventory = &server.player.inventory;
	inventory_set_hotbar(&server.player.inventory, 0);
	inventory_set_slot(
		&server.player.inventory, INVENTORY_SLOT_HOTBAR,
		(struct item_data) {.id = BLOCK_STONE, .count = 3});
	const w_coord_t barrier_x = 60, barrier_y = 70, barrier_z = 4;
	assert(server_world_set_block(
		&server.worlds[0], barrier_x, barrier_y, barrier_z,
		(struct block_data) {.type = BLOCK_STONE}));
	double barrier_px = server.player.x;
	double barrier_py = server.player.y;
	double barrier_pz = server.player.z;
	size_t barrier_entities = dict_entity_size(server.entities);
	struct server_rpc blocked_pos = {
		.type = SRPC_PLAYER_POS,
		.payload.player_pos = {
			.x = barrier_px + 100.0,
			.y = barrier_py + 20.0,
			.z = barrier_pz - 100.0,
		},
	};
	struct server_rpc blocked_dig = {
		.type = SRPC_BLOCK_DIG,
		.payload.block_dig = {
			.finished = true,
			.x = barrier_x,
			.y = barrier_y,
			.z = barrier_z,
		},
	};
	struct server_rpc blocked_drop = {
		.type = SRPC_DROP_ITEM,
		.payload.drop_item.all = true,
	};
	struct server_rpc blocked_hotbar = {
		.type = SRPC_HOTBAR_SLOT,
		.payload.hotbar_slot.slot = 8,
	};
	struct server_rpc blocked_unload = {.type = SRPC_UNLOAD_WORLD};
	server_local_test_process_rpc(&server, &blocked_pos);
	server_local_test_process_rpc(&server, &blocked_dig);
	server_local_test_process_rpc(&server, &blocked_drop);
	server_local_test_process_rpc(&server, &blocked_hotbar);
	server_local_test_process_rpc(&server, &blocked_unload);
	assert(server.player.x == barrier_px && server.player.y == barrier_py
		   && server.player.z == barrier_pz);
	assert(server.player.has_pos && server.player.finished_loading);
	assert(server.player.dimension == WORLD_DIM_OVERWORLD
		   && server.world.dimension == WORLD_DIM_NETHER);
	assert(inventory_get_hotbar(&server.player.inventory) == 0);
	assert(inventory_get_slot(&server.player.inventory,
							  INVENTORY_SLOT_HOTBAR, &observed));
	assert(observed.id == BLOCK_STONE && observed.count == 3);
	assert(dict_entity_size(server.entities) == barrier_entities);
	assert(server_world_get_block(&server.world, barrier_x, barrier_y,
								  barrier_z, &removed));
	assert(removed.type == BLOCK_STONE);
	server.death_world_evict = false;
	server.death_world_retry_delay = 0;

	dict_entity_it_t it;
	dict_entity_it(it, server.entities);
	while(!dict_entity_end_p(it)) {
		free(dict_entity_ref(it)->value);
		dict_entity_next(it);
	}
	dict_entity_clear(server.entities);
	inventory_destroy(&server.player.inventory, &server.player);
	server_world_discard(&server.world);
	string_clear(window_level);
	puts("  portal: window open/close ownership is allocation-transactional");
}
// Clear a box back to air using raw writes (editing_blocks on: no cascades,
// so teardown between cases cannot trip portal self-break mid-wipe).
static void wipe(int x0, int y0, int z0, int x1, int y1, int z1) {
	s.world.editing_blocks = true;
	for(int x = x0; x <= x1; x++)
		for(int y = y0; y <= y1; y++)
			for(int z = z0; z <= z1; z++)
				if(gid(x, y, z) != BLOCK_AIR)
					set(x, y, z, BLOCK_AIR, 0);
	s.world.editing_blocks = false;
}

struct portal_find_player_result {
	double x, y, z;
	float fall_distance, rx, ry, vel_x, vel_y, old_vel_y, vel_z;
};

static void portal_find_reset_player(struct server_local* server) {
	server->player.x = 0.25;
	server->player.y = 64.25;
	server->player.z = 0.25;
	server->player.rx = 123.0f;
	server->player.ry = -37.0f;
	server->player.vel_x = 0.375f;
	server->player.vel_y = -0.625f;
	server->player.old_vel_y = -0.5f;
	server->player.vel_z = 0.875f;
	server->player.fall_distance = 27.5F;
}

static struct portal_find_player_result portal_find_player_result(
	const struct server_local* server) {
	return (struct portal_find_player_result) {
		.x = server->player.x,
		.y = server->player.y,
		.z = server->player.z,
		.fall_distance = server->player.fall_distance,
		.rx = server->player.rx,
		.ry = server->player.ry,
		.vel_x = server->player.vel_x,
		.vel_y = server->player.vel_y,
		.old_vel_y = server->player.old_vel_y,
		.vel_z = server->player.vel_z,
	};
}

static void assert_portal_find_player_bits_equal(
	const struct portal_find_player_result* a,
	const struct portal_find_player_result* b) {
#define ASSERT_SAME_BITS(field)                                                \
	assert(memcmp(&a->field, &b->field, sizeof(a->field)) == 0)
	ASSERT_SAME_BITS(x);
	ASSERT_SAME_BITS(y);
	ASSERT_SAME_BITS(z);
	ASSERT_SAME_BITS(fall_distance);
	ASSERT_SAME_BITS(rx);
	ASSERT_SAME_BITS(ry);
	ASSERT_SAME_BITS(vel_x);
	ASSERT_SAME_BITS(vel_y);
	ASSERT_SAME_BITS(old_vel_y);
	ASSERT_SAME_BITS(vel_z);
#undef ASSERT_SAME_BITS
}

static void portal_test_set_region_compression_type(const char* path,
											 uint8_t type) {
	FILE* file = fopen(path, "rb+");
	assert(file);
	uint8_t index[4];
	assert(fread(index, sizeof(index), 1, file) == 1);
	uint32_t offset = ((uint32_t)index[0] << 16)
		| ((uint32_t)index[1] << 8) | (uint32_t)index[2];
	assert(offset >= 2);
	assert(offset <= (uint32_t)(LONG_MAX / REGION_SECTOR_SIZE));
	assert(fseek(file, (long)offset * REGION_SECTOR_SIZE + 4, SEEK_SET) == 0);
	assert(fwrite(&type, sizeof(type), 1, file) == 1);
	assert(fflush(file) == 0);
	assert(fclose(file) == 0);
}

static void portal_test_transcode_region_to_gzip(const char* path) {
	FILE* file = fopen(path, "rb+");
	assert(file);
	uint8_t index[4];
	assert(fread(index, sizeof(index), 1, file) == 1);
	uint32_t offset = ((uint32_t)index[0] << 16)
		| ((uint32_t)index[1] << 8) | (uint32_t)index[2];
	uint32_t sectors = index[3];
	assert(offset >= 2 && sectors >= 1);
	assert(offset <= (uint32_t)(LONG_MAX / REGION_SECTOR_SIZE));
	assert(fseek(file, (long)offset * REGION_SECTOR_SIZE, SEEK_SET) == 0);
	uint8_t header[5];
	assert(fread(header, sizeof(header), 1, file) == 1);
	uint32_t length = ((uint32_t)header[0] << 24)
		| ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8)
		| (uint32_t)header[3];
	assert(length >= 2 && header[4] == 2);
	uint8_t* zlib_data = malloc(length - 1);
	assert(zlib_data);
	assert(fread(zlib_data, length - 1, 1, file) == 1);

	uLongf raw_length = 1024u * 1024u;
	uint8_t* raw = malloc(raw_length);
	assert(raw);
	assert(uncompress(raw, &raw_length, zlib_data, length - 1) == Z_OK);
	free(zlib_data);

	uLong gzip_capacity = compressBound(raw_length) + 32u;
	uint8_t* gzip_data = malloc(gzip_capacity);
	assert(gzip_data);
	z_stream stream = {
		.next_in = raw,
		.avail_in = (uInt)raw_length,
		.next_out = gzip_data,
		.avail_out = (uInt)gzip_capacity,
	};
	assert(deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
			   MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY)
		   == Z_OK);
	assert(deflate(&stream, Z_FINISH) == Z_STREAM_END);
	size_t gzip_length = stream.total_out;
	assert(deflateEnd(&stream) == Z_OK);
	free(raw);
	assert(gzip_length + 5 <= (size_t)sectors * REGION_SECTOR_SIZE);
	assert(gzip_length <= UINT32_MAX - 1);
	uint32_t record_length = (uint32_t)gzip_length + 1;
	uint8_t record_header[5] = {
		(uint8_t)(record_length >> 24),
		(uint8_t)(record_length >> 16),
		(uint8_t)(record_length >> 8),
		(uint8_t)record_length,
		1,
	};
	assert(fseek(file, (long)offset * REGION_SECTOR_SIZE, SEEK_SET) == 0);
	assert(fwrite(record_header, sizeof(record_header), 1, file) == 1);
	assert(fwrite(gzip_data, gzip_length, 1, file) == 1);
	free(gzip_data);
	assert(fflush(file) == 0);
	assert(fclose(file) == 0);
}

static void portal_test_name_gzip_region_root(const char* path) {
	FILE* file = fopen(path, "rb+");
	assert(file);
	uint8_t index[4];
	assert(fread(index, sizeof(index), 1, file) == 1);
	uint32_t offset = ((uint32_t)index[0] << 16)
		| ((uint32_t)index[1] << 8) | (uint32_t)index[2];
	uint32_t sectors = index[3];
	assert(offset >= 2 && sectors >= 1);
	assert(offset <= (uint32_t)(LONG_MAX / REGION_SECTOR_SIZE));
	assert(fseek(file, (long)offset * REGION_SECTOR_SIZE, SEEK_SET) == 0);
	uint8_t header[5];
	assert(fread(header, sizeof(header), 1, file) == 1);
	uint32_t length = ((uint32_t)header[0] << 24)
		| ((uint32_t)header[1] << 16) | ((uint32_t)header[2] << 8)
		| (uint32_t)header[3];
	assert(length >= 2 && header[4] == 1);
	uint8_t* compressed = malloc(length - 1);
	assert(compressed);
	assert(fread(compressed, length - 1, 1, file) == 1);

	const size_t raw_capacity = 1024u * 1024u;
	uint8_t* raw = malloc(raw_capacity);
	assert(raw);
	z_stream inflate_stream = {
		.next_in = compressed,
		.avail_in = (uInt)(length - 1),
		.next_out = raw,
		.avail_out = (uInt)raw_capacity,
	};
	assert(inflateInit2(&inflate_stream, MAX_WBITS + 16) == Z_OK);
	assert(inflate(&inflate_stream, Z_FINISH) == Z_STREAM_END);
	size_t raw_length = inflate_stream.total_out;
	assert(inflate_stream.avail_in == 0);
	assert(inflateEnd(&inflate_stream) == Z_OK);
	free(compressed);
	assert(raw_length >= 3 && raw_length < raw_capacity);
	assert(raw[0] == TAG_COMPOUND && raw[1] == 0 && raw[2] == 0);
	memmove(raw + 4, raw + 3, raw_length - 3);
	raw[1] = 0;
	raw[2] = 1;
	raw[3] = 'X';
	raw_length++;

	uLong gzip_capacity = compressBound(raw_length) + 32u;
	uint8_t* gzip_data = malloc(gzip_capacity);
	assert(gzip_data);
	z_stream deflate_stream = {
		.next_in = raw,
		.avail_in = (uInt)raw_length,
		.next_out = gzip_data,
		.avail_out = (uInt)gzip_capacity,
	};
	assert(deflateInit2(&deflate_stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
			   MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY)
		   == Z_OK);
	assert(deflate(&deflate_stream, Z_FINISH) == Z_STREAM_END);
	size_t gzip_length = deflate_stream.total_out;
	assert(deflateEnd(&deflate_stream) == Z_OK);
	free(raw);
	assert(gzip_length + 5 <= (size_t)sectors * REGION_SECTOR_SIZE);
	assert(gzip_length <= UINT32_MAX - 1);
	uint32_t record_length = (uint32_t)gzip_length + 1;
	uint8_t record_header[5] = {
		(uint8_t)(record_length >> 24),
		(uint8_t)(record_length >> 16),
		(uint8_t)(record_length >> 8),
		(uint8_t)record_length,
		1,
	};
	assert(fseek(file, (long)offset * REGION_SECTOR_SIZE, SEEK_SET) == 0);
	assert(fwrite(record_header, sizeof(record_header), 1, file) == 1);
	assert(fwrite(gzip_data, gzip_length, 1, file) == 1);
	free(gzip_data);
	assert(fflush(file) == 0);
	assert(fclose(file) == 0);
}

static enum region_archive_read_result portal_test_read_region_ids(
	string_t level_name, uint8_t* ids) {
	struct region_archive archive;
	assert(region_archive_create(
		&archive, level_name, 0, 0, WORLD_DIM_OVERWORLD));
	enum region_archive_read_result result
		= region_archive_get_block_ids(&archive, 0, 0, ids);
	region_archive_destroy(&archive);
	return result;
}

/* The MCR type byte is an instruction, not a hint.  The portal-only reader
 * must reject unsupported types and a gzip label on a zlib payload instead of
 * letting cNBT auto-detection silently accept it. */
static void test_portal_region_blocks_reader_types(void) {
	char world_path[128];
	char region_path[192];
	test_fixture_dir("test_portal_region_reader", world_path,
					  sizeof world_path);
	snprintf(region_path, sizeof region_path, "%s/region/r.0.0.mcr",
			 world_path);
	MKDIR(world_path);
	MKDIR(test_fixture_join(world_path, "/region"));
	remove(region_path);

	string_t level_name;
	string_init_set_str(level_name, world_path);
	struct server_world world;
	server_world_create(
		&world, level_name, WORLD_DIM_OVERWORLD, 0x173, true);
	assert(world.local == NULL);
	struct server_chunk* chunk = NULL;
	assert(server_world_load_chunk(&world, 0, 0, &chunk) && chunk);
	size_t marker = 17
		+ ((size_t)3 + (size_t)2 * CHUNK_SIZE) * WORLD_HEIGHT;
	chunk->ids[marker] = BLOCK_PORTAL;
	chunk->modified = true;
	assert(server_world_checkpoint_chunks(&world));
	server_world_discard(&world);

	uint8_t* ids = malloc(CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT);
	assert(ids);
	assert(portal_test_read_region_ids(level_name, ids)
		   == REGION_ARCHIVE_READ_OK);
	assert(ids[marker] == BLOCK_PORTAL);

	portal_test_set_region_compression_type(region_path, 0);
	assert(portal_test_read_region_ids(level_name, ids)
		   == REGION_ARCHIVE_READ_ERROR);
	portal_test_set_region_compression_type(region_path, 3);
	assert(portal_test_read_region_ids(level_name, ids)
		   == REGION_ARCHIVE_READ_ERROR);
	portal_test_set_region_compression_type(region_path, 1);
	assert(portal_test_read_region_ids(level_name, ids)
		   == REGION_ARCHIVE_READ_ERROR);
	portal_test_set_region_compression_type(region_path, 2);
	assert(portal_test_read_region_ids(level_name, ids)
		   == REGION_ARCHIVE_READ_OK);
	assert(ids[marker] == BLOCK_PORTAL);
	portal_test_transcode_region_to_gzip(region_path);
	assert(portal_test_read_region_ids(level_name, ids)
		   == REGION_ARCHIVE_READ_OK);
	assert(ids[marker] == BLOCK_PORTAL);
	portal_test_set_region_compression_type(region_path, 2);
	assert(portal_test_read_region_ids(level_name, ids)
		   == REGION_ARCHIVE_READ_ERROR);
	portal_test_set_region_compression_type(region_path, 1);
	assert(portal_test_read_region_ids(level_name, ids)
		   == REGION_ARCHIVE_READ_OK);
	portal_test_name_gzip_region_root(region_path);
	assert(portal_test_read_region_ids(level_name, ids)
		   == REGION_ARCHIVE_READ_ERROR);

	/* A named root must fail during FIND itself. It must never become a portal
	 * winner and reach the full chunk loader/residency graph. */
	struct server_local malformed = {0};
	server_world_create(
		&malformed.world, level_name, WORLD_DIM_OVERWORLD, 0x173, true);
	malformed.worlds[0].session = &malformed; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	malformed.player.x = 2.0;
	malformed.player.y = 17.0;
	malformed.player.z = 3.0;
	unsigned default_radius = portal_teleporter_test_default_find_radius();
	portal_teleporter_test_set_find_radius(0);
	portal_teleporter_test_find_stats_reset();
	struct java_random rng;
	jrand_set_seed(&rng, 0x173);
	struct portal_teleporter_job* job
		= portal_teleporter_begin(&malformed, &rng);
	assert(job);
	enum portal_teleporter_result result = PORTAL_TELEPORTER_WORKING;
	for(int attempt = 0; attempt < 8; attempt++) {
		result = portal_teleporter_step(&malformed, job);
		assert(result == (attempt < 7 ? PORTAL_TELEPORTER_WORKING
									 : PORTAL_TELEPORTER_FAILED));
		assert(dict_server_chunks_size(malformed.world.chunks) == 0);
	}
	struct portal_teleporter_find_stats stats;
	portal_teleporter_test_find_stats_get(&stats);
	assert(!stats.completed && stats.find_slices == 8);
	assert(stats.find_columns == 0 && stats.raw_id_reads == 0);
	portal_teleporter_destroy(job);
	portal_teleporter_test_set_find_radius(default_radius);
	server_world_discard(&malformed.world);

	free(ids);
	string_clear(level_name);
	remove(region_path);
	test_fixture_dir_cleanup(world_path);
	puts("  region: bounded Blocks reader rejects bad MCR types/wrappers/root OK");
}

static void portal_test_archive_chunk_init(struct server_chunk* chunk) {
	memset(chunk, 0, sizeof(*chunk));
	const size_t ids_size = CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
	chunk->ids = calloc(1, ids_size);
	chunk->metadata = calloc(1, ids_size / 2);
	chunk->lighting_sky = calloc(1, ids_size / 2);
	chunk->lighting_torch = calloc(1, ids_size / 2);
	chunk->heightmap = calloc(1, CHUNK_SIZE * CHUNK_SIZE);
	assert(chunk->ids && chunk->metadata && chunk->lighting_sky
		   && chunk->lighting_torch && chunk->heightmap);
	chunk->modified = true;
	chunk->populated = true;
}

static void portal_test_archive_chunk_clear(struct server_chunk* chunk) {
	free(chunk->ids);
	free(chunk->metadata);
	free(chunk->lighting_sky);
	free(chunk->lighting_torch);
	free(chunk->heightmap);
}

static size_t portal_test_chunk_index(
	w_coord_t x, w_coord_t y, w_coord_t z) {
	return (size_t)y
		+ ((size_t)W2C_COORD(z)
		   + (size_t)W2C_COORD(x) * CHUNK_SIZE) * WORLD_HEIGHT;
}

static void portal_test_write_archive_chunk(
	string_t level_name, w_coord_t cx, w_coord_t cz,
	struct server_chunk* chunk, const struct chunk_tile_entities* te,
	const struct chunk_entities* entities) {
	struct region_archive archive;
	assert(region_archive_create_new(
		&archive, level_name, CHUNK_REGION_COORD(cx), CHUNK_REGION_COORD(cz),
		WORLD_DIM_OVERWORLD));
	assert(region_archive_set_chunk(
		&archive, cx, cz, chunk, te, entities));
	region_archive_destroy(&archive);
}

static bool portal_test_all_chests_free(const struct server_local* server) {
	for(int i = 0; i < MAX_CHESTS; i++)
		if(server->chest_pos[i].y >= 0)
			return false;
	return true;
}

/* Disk FIND must remain a read-only query over the residency graph. Two saved
 * chunks carry three equidistant negative-coordinate portal candidates plus a
 * decoy Entity/TileEntities payload. The search keeps zero chunks/entities/TEs
 * resident until it has selected Beta's x-asc,z-asc,y-desc winner, then loads
 * only that winning chunk. */
static void test_portal_find_disk_query_only(void) {
	char world_path[128];
	char winner_region[192];
	char decoy_region[192];
	test_fixture_dir("test_portal_find_disk", world_path, sizeof world_path);
	snprintf(winner_region, sizeof winner_region, "%s/region/r.-1.-1.mcr",
			 world_path);
	snprintf(decoy_region, sizeof decoy_region, "%s/region/r.0.0.mcr",
			 world_path);
	MKDIR(world_path);
	MKDIR(test_fixture_join(world_path, "/region"));
	remove(winner_region);
	remove(decoy_region);
	string_t level_name;
	string_init_set_str(level_name, world_path);

	struct server_chunk winner;
	portal_test_archive_chunk_init(&winner);
	const w_coord_t winner_x = -13, winner_z = -12;
	const w_coord_t lower_y = 62, winner_y = 65;
	winner.ids[portal_test_chunk_index(winner_x, lower_y, winner_z)]
		= BLOCK_PORTAL;
	winner.ids[portal_test_chunk_index(winner_x, winner_y, winner_z)]
		= BLOCK_PORTAL;
	/* Same distance as the high winner after swapping x/z. x=-13 wins first. */
	winner.ids[portal_test_chunk_index(-12, winner_y, -13)] = BLOCK_PORTAL;
	portal_test_write_archive_chunk(
		level_name, -1, -1, &winner, NULL, NULL);
	portal_test_archive_chunk_clear(&winner);

	struct server_chunk decoy;
	portal_test_archive_chunk_init(&decoy);
	decoy.ids[portal_test_chunk_index(5, 64, 5)] = BLOCK_CHEST;
	struct te_chest chest = {.x = 5, .y = 64, .z = 5};
	chest.items[0] = (struct item_data) {.id = ITEM_STICK, .count = 1};
	struct chunk_tile_entities te = {.chests = &chest, .n_chest = 1};
	struct archived_entity record = {
		.kind = ARCHIVED_ENTITY_ITEM,
		.source_id = 777,
		.pos = {5.5f, 64.5f, 5.5f},
		.data.item = {
			.item = {.id = ITEM_STICK, .count = 1},
			.age = 7,
		},
	};
	struct chunk_entities entities = {.entities = &record, .count = 1};
	portal_test_write_archive_chunk(
		level_name, 0, 0, &decoy, &te, &entities);
	portal_test_archive_chunk_clear(&decoy);

	struct server_local server = {0};
	dict_entity_init(server.entities);
	/* IMP-TE-CAPS: growable tile slabs (all rows free) */
	if(!server_local_te_tables_init(&server))
		abort();
	piston_moving_store_init(&server.piston_moving);
	server_world_create(
		&server.world, level_name, WORLD_DIM_OVERWORLD, 0x173, true);
	server.world.local = &server;
	server.worlds[0].session = &server; /* A1a (D1): owning session for services/callbacks */
	server.player.x = 0.0;
	server.player.y = 64.0;
	server.player.z = 0.0;
	server.player.vel_x = 0.25f;
	server.player.vel_y = -0.5f;
	server.player.old_vel_y = -0.25f;
	server.player.vel_z = 0.75f;
	server.player.fall_distance = 16.0F;

	unsigned default_radius = portal_teleporter_test_default_find_radius();
	portal_teleporter_test_set_find_radius(20);
	portal_teleporter_test_find_stats_reset();
	struct java_random rng;
	jrand_set_seed(&rng, 0x173);
	struct portal_teleporter_job* job
		= portal_teleporter_begin(&server, &rng);
	assert(job);
	enum portal_teleporter_result result = PORTAL_TELEPORTER_WORKING;
	struct portal_teleporter_find_stats stats = {0};
	for(int step = 0; step < 64 && !stats.completed; step++) {
		result = portal_teleporter_step(&server, job);
		assert(result == PORTAL_TELEPORTER_WORKING);
		portal_teleporter_test_find_stats_get(&stats);
		assert(dict_server_chunks_size(server.world.chunks) == 0);
		assert(dict_entity_size(server.entities) == 0);
		assert(portal_test_all_chests_free(&server));
	}
	assert(stats.completed);
	for(int step = 0; step < 8 && result == PORTAL_TELEPORTER_WORKING;
		step++) {
		result = portal_teleporter_step(&server, job);
		assert(!server_world_is_chunk_loaded(&server.world, 0, 0));
		assert(dict_entity_size(server.entities) == 0);
		assert(portal_test_all_chests_free(&server));
	}
	assert(result == PORTAL_TELEPORTER_DONE);
	assert(dict_server_chunks_size(server.world.chunks) == 1);
	assert(server_world_is_chunk_loaded(&server.world, -1, -1));
	assert(!server_world_is_chunk_loaded(&server.world, 0, 0));
	assert(server.player.x == (double)winner_x + 0.5);
	assert(server.player.y
		   == (double)winner_y + 0.5 + PLAYER_EYE_HEIGHT);
	assert(server.player.z == (double)winner_z + 0.5);
	assert(server.player.vel_x == 0.0f && server.player.vel_y == 0.0f
		   && server.player.vel_z == 0.0f);
	assert(server.player.old_vel_y == 0.0f);
	/* Minecraft.usePortal reuses the EntityPlayer. Teleporter zeroes motion but
	 * setLocationAndAngles does not clear Entity.fallDistance. */
	assert(server.player.fall_distance == 16.0F);

	portal_teleporter_destroy(job);
	portal_teleporter_test_set_find_radius(default_radius);
	server_world_discard(&server.world);
	piston_moving_store_destroy(&server.piston_moving);
	dict_entity_clear(server.entities);
	string_clear(level_name);
	remove(winner_region);
	remove(decoy_region);
	test_fixture_dir_cleanup(world_path);
	puts("  portal: disk FIND is nonresident/entity-free with Beta negative tie order OK");
}

/* Regression for the Wii portal stall. A radius-12 window stays inside four
 * already-loaded chunks while retaining Beta's x-ascending, z-ascending,
 * y-descending winner order under the chunk-at-a-time scan. The
 * only portal sits in the penultimate column at y=0 (x!=z catches a transposed
 * raw-column index), so all 625 columns must read exactly 128 ids before the
 * cooperative result can be compared
 * byte-for-byte with the direct Teleporter.func_4106_b transcription.
 *
 * The production-radius arithmetic is asserted independently: 257 columns
 * span exactly 17 chunks on either axis, but those 289 chunks must never become
 * simultaneously resident merely to search their Blocks arrays.
 */
static void test_portal_find_raw_column_red(void) {
	const unsigned default_radius
		= portal_teleporter_test_default_find_radius();
	const uint64_t production_width = (uint64_t)default_radius * 2u + 1u;
	const uint64_t production_columns = production_width * production_width;
	assert(default_radius == 128);
	assert(production_columns == 66049u);
	assert(WCOORD_CHUNK_OFFSET((w_coord_t)default_radius)
		   - WCOORD_CHUNK_OFFSET(-(w_coord_t)default_radius) + 1 == 17);

	static const unsigned radius = 12;
	static const w_coord_t portal_x = 12, portal_y = 0, portal_z = 11;
	const uint64_t fixture_columns = (radius * 2u + 1u)
		* (radius * 2u + 1u);
	const uint64_t fixture_raw_reads
		= fixture_columns * WORLD_HEIGHT;
	portal_teleporter_test_set_find_radius(radius);

	char world_path[128];
	test_fixture_dir("test_portal_find_perf", world_path, sizeof world_path);
	MKDIR(world_path);
	string_t level_name;
	string_init_set_str(level_name, world_path);
	struct server_local server = {0};
	server_world_create(
		&server.world, level_name, WORLD_DIM_OVERWORLD, 99173, true);
	server.worlds[0].session = &server; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	struct server_chunk* chunk = NULL;
	for(w_coord_t cx = -1; cx <= 0; cx++)
		for(w_coord_t cz = -1; cz <= 0; cz++)
			assert(server_world_load_chunk(
				&server.world, cx, cz, &chunk) && chunk);
	chunk = dict_server_chunks_get(
		server.world.chunks,
		S_CHUNK_ID(WCOORD_CHUNK_OFFSET(portal_x),
				   WCOORD_CHUNK_OFFSET(portal_z)));
	assert(chunk && chunk->ids);
	size_t portal_index = (size_t)portal_y
		+ ((size_t)W2C_COORD(portal_z)
		   + (size_t)W2C_COORD(portal_x) * CHUNK_SIZE) * WORLD_HEIGHT;
	chunk->ids[portal_index] = BLOCK_PORTAL;

	portal_find_reset_player(&server);
	struct java_random rng;
	jrand_set_seed(&rng, 0x173);
	portal_teleporter_test_find_stats_reset();
	struct portal_teleporter_job* job
		= portal_teleporter_begin(&server, &rng);
	assert(job);
	enum portal_teleporter_result result = PORTAL_TELEPORTER_WORKING;
	for(int step = 0; step < 16 && result == PORTAL_TELEPORTER_WORKING;
		step++)
		result = portal_teleporter_step(&server, job);
	assert(result == PORTAL_TELEPORTER_DONE);
	portal_teleporter_destroy(job);

	struct portal_teleporter_find_stats stats;
	portal_teleporter_test_find_stats_get(&stats);
	assert(stats.completed);
	assert(stats.find_columns == fixture_columns);
	assert(stats.find_slices >= 1 && stats.find_slices <= 4);
	assert(stats.max_columns_per_slice > 0
		   && stats.max_columns_per_slice <= fixture_columns);
	assert(stats.chunk_resolves == 4); /* one resolve per intersecting chunk */
	assert(stats.raw_id_reads == fixture_raw_reads);
	assert(stats.world_get_block_calls <= 4);
	struct portal_find_player_result cooperative
		= portal_find_player_result(&server);

	portal_find_reset_player(&server);
	jrand_set_seed(&rng, 0x173);
	portal_teleporter_place(&server, &rng);
	struct portal_find_player_result oracle
		= portal_find_player_result(&server);
	assert_portal_find_player_bits_equal(&cooperative, &oracle);

	/* Beta CREATE pass 2 uses orientation parity only: starts 0 and 2 both
	 * validate the same positive-Z strip. The cooperative helper used to apply
	 * pass 1's %4 reversal to start 2, probing z-2 instead of z+2 and then
	 * building into terrain it never validated. Make only the positive strip
	 * valid so that directional drift fails independently of the full search. */
	const w_coord_t site_x = 4, site_y = 100, site_z = 4;
	chunk = dict_server_chunks_get(
		server.world.chunks, S_CHUNK_ID(0, 0));
	assert(chunk && chunk->ids);
	for(w_coord_t dz = -2; dz <= 2; dz++)
		for(w_coord_t dy = 0; dy <= 3; dy++) {
			size_t idx = (size_t)(site_y + dy)
				+ ((size_t)W2C_COORD(site_z + dz)
				   + (size_t)W2C_COORD(site_x) * CHUNK_SIZE)
					* WORLD_HEIGHT;
			chunk->ids[idx] = BLOCK_AIR;
		}
	for(w_coord_t dz = -1; dz <= 2; dz++) {
		size_t idx = (size_t)(site_y - 1)
			+ ((size_t)W2C_COORD(site_z + dz)
			   + (size_t)W2C_COORD(site_x) * CHUNK_SIZE)
				* WORLD_HEIGHT;
		chunk->ids[idx] = BLOCK_STONE;
	}
	assert(portal_teleporter_test_create_volume_clear(
		&server, site_x, site_y, site_z, 0, true));
	assert(portal_teleporter_test_create_volume_clear(
		&server, site_x, site_y, site_z, 2, true));

	portal_teleporter_test_set_find_radius(default_radius);
	server_world_discard(&server.world);
	string_clear(level_name);
	test_fixture_dir_cleanup(world_path);
	puts("  portal: raw-column FIND work bound + synchronous oracle OK");
}


int main(void) {
	test_fixture_dir("test_portal_tmp", s_fixture_TMP_WORLD,
					  sizeof s_fixture_TMP_WORLD);
	test_fixture_dir("test_portal_tmp2", s_fixture_test_portal_tmp2,
					  sizeof s_fixture_test_portal_tmp2);
	test_portal_tmp2_paths_init();

	assert(clin_init());
	assert(svin_init());
	blocks_init();
	items_init();
	test_initial_load_scratch_oom_reports_terminal();
	test_portal_find_raw_column_red();
	test_portal_region_blocks_reader_types();
	test_portal_find_disk_query_only();
	test_stale_window_rpcs_do_not_cross_world_load();
	test_reserved_close_transport();
	test_pause_toggle_transport();
	test_dictionary_transaction_thread_ownership();
	test_input_ack_transport_gate();
	test_live_stream_recovery_server();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	MKDIR(TMP_WORLD);
	test_transactional_window_open();
	test_shared_chest_actors();
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 99, true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	// ============ I1: X-axis frame (corners absent) ignites to the exact grid ============
	{
		const int ax = 4, az = 4;
		build_frame(ax, Y, az, 1, 0, -1);
		// flint&steel analog: fire onto the bottom border, i.e. into the lower-left
		// interior cell (obsidian directly below at (ax, Y-1, az)).
		set(ax, Y, az, BLOCK_FIRE, 0);
		assert_portal_grid(ax, Y, az, 1, 0);
		printf("  portal: X-axis corners-exempt frame ignites to exact 2x3 grid OK\n");
	}

	// ============ I2: anchor shift -- fire in the RIGHT interior column, same grid ============
	{
		const int ax = 12, az = 4;
		build_frame(ax, Y, az, 1, 0, -1);
		set(ax + 1, Y, az, BLOCK_FIRE, 0); // (ax, Y, az) is air -> Beta shifts the anchor
		assert_portal_grid(ax, Y, az, 1, 0);
		printf("  portal: air-conditional anchor shift (fire in right column) OK\n");
	}

	// ============ I3: Z-axis frame ============
	{
		const int ax = 4, az = 12;
		build_frame(ax, Y, az, 0, 1, -1);
		set(ax, Y, az, BLOCK_FIRE, 0);
		assert_portal_grid(ax, Y, az, 0, 1);
		printf("  portal: Z-axis frame ignites OK\n");
	}

	// ============ I4: rejections -- missing border / occupied interior / both axes ============
	{
		// Missing SIDE border block: no portal, the fire itself persists (young fire
		// on obsidian survives its placement; Beta falls through to the survival path).
		const int ax = 20, az = 4;
		build_frame(ax, Y, az, 1, 0, 4); // skip the top border over the left column
		set(ax, Y, az, BLOCK_FIRE, 0);
		assert(count_portal(ax - 2, Y - 2, az - 2, ax + 3, Y + 4, az + 2) == 0);
		assert(gid(ax, Y, az) == BLOCK_FIRE);
		wipe(ax - 2, Y - 2, az - 2, ax + 3, Y + 4, az + 2);

		// Occupied interior (stone in the upper interior): no portal.
		build_frame(ax, Y, az, 1, 0, -1);
		set(ax + 1, Y + 2, az, BLOCK_STONE, 0);
		set(ax, Y, az, BLOCK_FIRE, 0);
		assert(count_portal(ax - 2, Y - 2, az - 2, ax + 3, Y + 4, az + 2) == 0);
		wipe(ax - 2, Y - 2, az - 2, ax + 3, Y + 4, az + 2);

		// Obsidian on BOTH horizontal axes of the fire cell: var5 == var6 -> fail.
		set(ax - 1, Y, az, BLOCK_OBSIDIAN, 0);
		set(ax, Y, az - 1, BLOCK_OBSIDIAN, 0);
		set(ax, Y - 1, az, BLOCK_OBSIDIAN, 0); // below, so the hook fires at all
		set(ax, Y, az, BLOCK_FIRE, 0);
		assert(count_portal(ax - 2, Y - 2, az - 2, ax + 3, Y + 4, az + 2) == 0);
		wipe(ax - 2, Y - 2, az - 2, ax + 3, Y + 4, az + 2);
		printf("  portal: missing-border / occupied-interior / both-axes rejected OK\n");
	}

	// ==== I5: completing the frame around BURNING fire does NOT ignite (Beta-faithful) ====
	{
		const int ax = 4, az = 20;
		build_frame(ax, Y, az, 1, 0, 4); // one border block missing
		set(ax, Y, az, BLOCK_FIRE, 0);	 // burns, no portal yet (I4 proved this)
		assert(count_portal(ax - 2, Y - 2, az - 2, ax + 3, Y + 4, az + 2) == 0);
		build_frame(ax, Y, az, 1, 0, -1); // place the missing obsidian

		// Beta's fire AGE bump is setBlockMetadata -- a metadata-only write that
		// NEVER dispatches onBlockAdded [decompile 740c583 BlockFire.updateTick:66]
		// -- so the burning fire never re-attempts the portal. Two ticks (few
		// enough that the age-3 no-fuel death can't clear it), still no portal.
		for(int k = 0; k < 2; k++)
			fire_tick(ax, Y, az);
		assert(count_portal(ax - 2, Y - 2, az - 2, ax + 3, Y + 4, az + 2) == 0);

		// Quirk golden: igniting the OTHER interior column while the first still
		// burns ALSO fails -- the anchor shift is strictly AIR-conditional, so the
		// fire at (ax,Y,az) blocks the shift and the frame scan misanchors at ax+1
		// (its var7=-1 border probe lands on the fire column, not obsidian).
		set(ax + 1, Y, az, BLOCK_FIRE, 0);
		assert(count_portal(ax - 2, Y - 2, az - 2, ax + 3, Y + 4, az + 2) == 0);

		// Clear both fires and re-ignite fresh: NOW the shift works and it lights.
		set(ax, Y, az, BLOCK_AIR, 0);
		set(ax + 1, Y, az, BLOCK_AIR, 0);
		set(ax, Y, az, BLOCK_FIRE, 0);
		assert_portal_grid(ax, Y, az, 1, 0);
		printf("  portal: burning-fire no-reattempt + fire-blocks-shift quirk + re-ignite OK\n");
	}

	// ============ B1: breaking a frame obsidian cascades the whole pane to air ============
	{
		const int ax = 12, az = 20;
		build_frame(ax, Y, az, 1, 0, -1);
		set(ax, Y, az, BLOCK_FIRE, 0);
		assert_portal_grid(ax, Y, az, 1, 0);
		// Remove the bottom border under the left column -> floor rule fails there,
		// and the break notifies its way through the rest of the pane.
		set(ax, Y - 1, az, BLOCK_AIR, 0);
		assert(count_portal(ax - 2, Y - 2, az - 2, ax + 3, Y + 4, az + 2) == 0);
		printf("  portal: frame break cascades whole pane to air OK\n");
	}

	// ============ B2: lone unframed portal block breaks on any neighbour change ============
	{
		const int x = 20, z = 20;
		s.world.editing_blocks = true; // place without notify so it survives placement
		set(x, Y, z, BLOCK_PORTAL, 0);
		s.world.editing_blocks = false;
		assert(gid(x, Y, z) == BLOCK_PORTAL);
		set(x + 1, Y, z, BLOCK_STONE, 0); // any neighbour change triggers validation
		assert(gid(x, Y, z) == BLOCK_AIR);
		wipe(x, Y, z, x + 1, Y, z);
		printf("  portal: lone portal block breaks on neighbour change OK\n");
	}

	// ============ B3: 4-tall portal column violates the exactly-3 rule ============
	{
		const int x = 26, z = 12;
		s.world.editing_blocks = true;
		set(x, Y - 1, z, BLOCK_OBSIDIAN, 0); // obsidian floor
		for(int j = 0; j < 4; j++)			 // 4-tall portal column (no cap)
			set(x, Y + j, z, BLOCK_PORTAL, 0);
		s.world.editing_blocks = false;
		set(x + 1, Y, z, BLOCK_STONE, 0); // poke -> bottom cell validates: height 4 != 3
		assert(count_portal(x, Y, z, x, Y + 3, z) == 0);
		printf("  portal: 4-tall column breaks (exactly-3-tall rule) OK\n");
	}

	// ============ D1: the dwell state machine (Beta ramp/decay/cooldown) ============
	{
		struct server_player p;
		memset(&p, 0, sizeof(p));
		p.time_until_portal = 20; // Beta EntityPlayer ctor default

		// Fresh-spawn pin: while the ctor cooldown runs, contact only REFRESHES
		// it to 10 -- it can never arm the ramp.
		for(int t = 0; t < 30; t++) {
			server_player_portal_tick(&p);
			server_player_set_in_portal(&p);
			assert(!p.in_portal && p.time_until_portal >= 9);
		}
		assert(!p.portal_travel_requested);

		// Step out: the cooldown drains 10 -> 0 in 10 ticks.
		for(int t = 0; t < 10; t++)
			server_player_portal_tick(&p);
		assert(p.time_until_portal == 0);

		// Continuous contact: Beta float accumulation 0.0125F/tick. Tick 1 arms
		// in_portal; the ramp consumes it from tick 2 on; the request latches
		// the tick the sum first reaches >= 1.0F. IEEE754 float truth (same in
		// Java): after 80 adds the accumulated sum is 0.99999994 -- just UNDER
		// 1.0F -- so the ramp needs EIGHTY-ONE adds. Beta 1.7.3's real dwell is
		// 81 contact ticks (~4.05 s), not the folkloric 80; +1 here for the
		// arming tick = latch at contact tick 82.
		int latched_at = -1;
		for(int t = 1; t <= 100 && latched_at < 0; t++) {
			server_player_portal_tick(&p);
			server_player_set_in_portal(&p);
			if(p.portal_travel_requested)
				latched_at = t;
		}
		assert(latched_at == 82);
		assert(p.time_in_portal == 1.0F);
		// The latching tick set the cooldown to 10, then its own trailing
		// decrement ran (Beta statement order) -> 9, then the arrival pin
		// keeps refreshing it to 10 while contact continues.
		assert(p.time_until_portal == 9 || p.time_until_portal == 10);

		// Arrival pin: standing in the destination portal refreshes the
		// cooldown forever; the ramp never re-arms, and timeInPortal DECAYS
		// (Beta's overlay fades even inside the frame).
		p.portal_travel_requested = false;
		for(int t = 0; t < 40; t++) {
			server_player_portal_tick(&p);
			server_player_set_in_portal(&p);
		}
		assert(!p.portal_travel_requested);
		assert(p.time_in_portal == 0.0F); // 1.0 / 0.05 = 20 ticks to fade
		printf("  portal: dwell machine (20-pin, 81-tick latch, arrival pin, decay) OK\n");
	}

	// ============ T1: Teleporter FIND -- nearest portal, centering, pitch/vel ============
	{
		// Two portal columns side by side (X-axis pair) near the player, one
		// farther single column: the near pair's LEFT column (x asc tie-break
		// inside the pair) must win, and the +0.5 centering lands the player
		// on the pair's shared edge.
		s.world.editing_blocks = true;
		for(int j = 0; j < 3; j++) {
			set(10, 20 + j, 10, BLOCK_PORTAL, 0);
			set(11, 20 + j, 10, BLOCK_PORTAL, 0);
			set(28, 30 + j, 14, BLOCK_PORTAL, 0); // decoy, farther away
		}
		s.world.editing_blocks = false;

		s.player.x = 12.3;
		s.player.y = 22.0;
		s.player.z = 10.7;
		s.player.rx = 123.0f;
		s.player.ry = 45.0f;
		s.player.vel_y = 3.0f;
		s.player.vel_x = 1.0f;
		s.player.vel_z = 1.0f;
		struct java_random rng;
		jrand_set_seed(&rng, 1234);
		portal_teleporter_place(&s, &rng);
		// Bottom of the near-left column = (10,20,10); portal at x+1 shifts
		// +0.5 -> x = 11.0 (the shared edge); eye y = 20.5 + PLAYER_EYE_HEIGHT.
		assert(s.player.x == 11.0);
		assert(s.player.y == 20.5 + PLAYER_EYE_HEIGHT);
		assert(s.player.z == 10.5);
		assert(s.player.rx == 123.0f); // yaw preserved
		assert(s.player.ry == 0.0f);   // pitch forced level
		assert(s.player.vel_x == 0.0f && s.player.vel_y == 0.0f
			   && s.player.vel_z == 0.0f);
		// wipe the fixtures (editing off would cascade portal self-breaks;
		// use the raw path)
		wipe(9, 19, 9, 12, 24, 11);
		wipe(27, 29, 13, 29, 34, 15);
		printf("  portal: teleporter FIND (nearest, shared-edge centering, pitch 0) OK\n");
	}

	// ============ T2: Teleporter CREATE -- corners-INCLUDED frame, player placed ============
	{
		// The ignition cases above left LIVE panes at y~70 (the FIND golden
		// really did reach them from 60+ blocks away -- proof of the 128
		// search radius, but wrong for this case): clear the whole earlier
		// staging band so NO portal exists and create must run.
		wipe(0, 65, 0, 31, 78, 31);
		// No portals anywhere now: create must build the 4x5 frame (14
		// obsidian + 6 portal, corners INCLUDED -- unlike player-lit frames)
		// on the superflat surface; the second FIND places the player inside.
		s.player.x = 16.0;
		s.player.y = 4.0 + PLAYER_EYE_HEIGHT;
		s.player.z = 16.0;
		s.player.rx = 0.0f;
		struct java_random rng;
		jrand_set_seed(&rng, 42);
		portal_teleporter_place(&s, &rng);

		// The player now stands in a portal: locate its pane and validate.
		int px = (int)floor(s.player.x), pz = (int)floor(s.player.z);
		int py = (int)floor(s.player.y - PLAYER_EYE_HEIGHT + 0.25);
		assert(gid(px, py, pz) == BLOCK_PORTAL);
		// Count the build in a generous box around the player: exactly 6
		// portal cells; the frame is corners-included = 14 obsidian.
		int portal_n = count_portal(px - 3, py - 2, pz - 3, px + 3, py + 5, pz + 3);
		assert(portal_n == 6);
		int obsidian_n = 0;
		for(int x = px - 3; x <= px + 3; x++)
			for(int y = py - 2; y <= py + 5; y++)
				for(int z = pz - 3; z <= pz + 3; z++)
					if(gid(x, y, z) == BLOCK_OBSIDIAN)
						obsidian_n++;
		if(obsidian_n != 14) {
			fprintf(stderr, "[DBG] obsidian_n=%d portal_n=%d player=(%f,%f,%f)\n",
					obsidian_n, portal_n, s.player.x, s.player.y, s.player.z);
			for(int y = py + 5; y >= py - 2; y--) {
				for(int z = pz - 4; z <= pz + 4; z++) {
					for(int x = px - 4; x <= px + 4; x++) {
						int id = gid(x, y, z);
						fputc(id == BLOCK_OBSIDIAN	  ? 'O'
							  : id == BLOCK_PORTAL	  ? 'P'
							  : id == BLOCK_AIR		  ? '.'
													  : '#',
							  stderr);
					}
					fputc(' ', stderr);
				}
				fputc('\n', stderr);
			}
		}
		assert(obsidian_n == 14);
		assert(s.player.ry == 0.0f);
		printf("  portal: teleporter CREATE (corners-included frame, placed inside) OK\n");
	}

	// A corrupt existing region must never be treated as a missing region by the
	// production world checkpoint path. The dirty generated chunk remains live,
	// checkpoint reports failure, and the original bytes stay untouched.
	{
		static const unsigned char corrupt_region[] = {
			0x43, 0x41, 0x56, 0x45, 0x57, 0x49, 0x49,
		};
		char corrupt_world_path[128];
		char corrupt_path[192];
		test_fixture_dir("test_portal_corrupt_region", corrupt_world_path,
						  sizeof corrupt_world_path);
		snprintf(corrupt_path, sizeof corrupt_path, "%s/region/r.0.0.mcr",
				 corrupt_world_path);
		MKDIR(corrupt_world_path);
		MKDIR(test_fixture_join(corrupt_world_path, "/region"));
		remove(corrupt_path);

		string_t corrupt_level;
		string_init_set_str(corrupt_level, corrupt_world_path);
		static struct server_world corrupt_world;
		server_world_create(&corrupt_world, corrupt_level, WORLD_DIM_OVERWORLD,
							4242, true);
		struct server_chunk* corrupt_chunk = NULL;
		assert(server_world_load_chunk(&corrupt_world, 0, 0, &corrupt_chunk)
			   && corrupt_chunk && corrupt_chunk->modified);
		FILE* corrupt_file = fopen(corrupt_path, "wb");
		assert(corrupt_file);
		assert(fwrite(corrupt_region, 1, sizeof(corrupt_region), corrupt_file)
			   == sizeof(corrupt_region));
		assert(fclose(corrupt_file) == 0);
		assert(!server_world_checkpoint_chunks(&corrupt_world));
		assert(corrupt_chunk->modified);

		unsigned char observed[sizeof(corrupt_region)] = {0};
		corrupt_file = fopen(corrupt_path, "rb");
		assert(corrupt_file);
		assert(fread(observed, 1, sizeof(observed), corrupt_file)
			   == sizeof(observed));
		assert(fgetc(corrupt_file) == EOF);
		assert(fclose(corrupt_file) == 0);
		assert(memcmp(observed, corrupt_region, sizeof(observed)) == 0);
		server_world_discard(&corrupt_world);

		// The read path also fails closed. Corrupt existing storage is not the
		// same as an absent chunk and must not be replaced with generated terrain.
		static struct server_world corrupt_load_world;
		server_world_create(&corrupt_load_world, corrupt_level,
							WORLD_DIM_OVERWORLD, 4242, true);
		corrupt_chunk = NULL;
		assert(!server_world_load_chunk(&corrupt_load_world, 0, 0,
										   &corrupt_chunk));
		assert(!corrupt_chunk
			   && !server_world_is_chunk_loaded(&corrupt_load_world, 0, 0));
		server_world_discard(&corrupt_load_world);
		memset(observed, 0, sizeof(observed));
		corrupt_file = fopen(corrupt_path, "rb");
		assert(corrupt_file);
		assert(fread(observed, 1, sizeof(observed), corrupt_file)
			   == sizeof(observed));
		assert(fgetc(corrupt_file) == EOF);
		assert(fclose(corrupt_file) == 0);
		assert(memcmp(observed, corrupt_region, sizeof(observed)) == 0);
		string_clear(corrupt_level);
		remove(corrupt_path);
		test_fixture_dir_cleanup(corrupt_world_path);
		printf("  region: corrupt existing file is preserved on checkpoint failure OK\n");
	}

	// ============ T3: full dimension-switch round trip (real level.dat) ============
	{
		// A second, fully-created server over a REAL level archive -- the live
		// switch round-trips player state through level.dat (Beta usePortal:
		// toggle dim, x/z scaled by 8, same-entity fire/air carry, Teleporter
		// arrival). Uses the threadless test constructor so every production field
		// has its default while this harness owns every update deterministically.
		static struct server_local s2;
		server_local_test_create_without_thread(
			&s2, true /*debug.give*/, false, false, false, false,
			true /*superflat*/, false);
		struct server_world_populate_scratch* session_populate_scratch
			= s2.world_populate_scratch;
		assert(session_populate_scratch);
		// Manual portal transaction steps use the same pause gate as before; the
		// real-stream helper temporarily lifts it around one owned update slice.
		s2.paused = true;
		MKDIR(TEST_PORTAL_TMP2);
		clear_source_regions();
		clear_destination_regions();
		clear_destination_legacy();
		string_t lvl2;
		string_init_set_str(lvl2, TEST_PORTAL_TMP2);
		{
			struct level_archive la;
			assert(level_archive_create_new(&la, lvl2, 4242,
											(vec3) {8.5f, 6.0f, 8.5f}));
			level_archive_destroy(&la); // create_new writes on destroy
		}
		assert(server_local_load_world_impl(&s2, lvl2));
		assert(s2.player.has_pos);
		assert(s2.player.dimension == WORLD_DIM_OVERWORLD);
		assert(s2.worlds[0].populate_scratch_storage == session_populate_scratch);
		assert(!s2.worlds[0].populate_scratch_owned);
		server_local_test_finish_load_inventory(&s2);
		struct item_data load_slot;
		assert(inventory_get_slot(&s2.player.inventory, INVENTORY_SLOT_HOTBAR,
								  &load_slot));
		/* debug.give kit sentinel: hotbar slot 0 is FLINT & STEEL since the
		   2026-07-28 full-inventory kit (was BLOCK_CHEST x64). */
		assert(load_slot.id == ITEM_FLINT_STEEL && load_slot.count == 1);

		// Deviation-#18 split: the lethal hit ALWAYS commits the dead state;
		// the fallible cursor-drop close gates the RESPAWN instead. Force the
		// cursor-drop allocation to fail and prove the request stays
		// retryable with the container state untouched -- no half-respawn,
		// the same no-partial-transaction invariant as before, one seam
		// later.
		struct inventory* lethal_window = calloc(1, sizeof(*lethal_window));
		assert(lethal_window);
		assert(inventory_create(lethal_window, &inventory_logic_crafting, &s2,
								&s2.player,
								CRAFTING_SIZE, 0, 0, 0));
		inventory_set_picked_item(
			lethal_window,
			(struct item_data) {.id = ITEM_BREAD, .durability = 9, .count = 1});
		s2.player.active_inventory = lethal_window;
		assert(!server_local_damage_player(&s2, MAX_PLAYER_HEALTH * 2, 0.0f,
									   0.0f));
		assert(s2.player.health <= 0);
		s2.player.finished_loading = true;
		s2.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
		server_local_test_fail_next_entity_alloc();
		assert(!server_local_respawn_request(&s2));
		assert(s2.player.health <= 0);
		assert(s2.player.active_inventory == lethal_window);
		assert(inventory_get_picked_item(lethal_window, &load_slot));
		assert(load_slot.id == ITEM_BREAD && load_slot.durability == 9
			   && load_slot.count == 1);
		/* No successful respawn here on purpose: the close-race sub-test
		 * below needs this same window still OPEN (its own retry proves the
		 * consumed injection), and its fixture revives the corpse by direct
		 * health write -- the retry-success half of the request lives in
		 * test_death_screen_env S5 and the minecart M1 golden. */

		// Exact async-close race: CLOSE fails, then an already-queued in-game Q
		// arrives before REOPEN renders. World actions must stay gated on the
		// authoritative container, or its later on_destroy restores the dropped
		// hotbar stack and duplicates it.
		bool finished_before_close_race = s2.player.finished_loading;
		s2.player.finished_loading = true;
		/* Deviation-#18: zero-init health reads as DEAD; be explicitly alive. */
		s2.player.health = MAX_PLAYER_HEALTH;
		struct item_data hotbar_before_close_race;
		assert(inventory_get_slot(&s2.player.inventory, INVENTORY_SLOT_HOTBAR,
								  &hotbar_before_close_race));
		struct server_rpc close_crafting = {
			.type = SRPC_WINDOW_CLOSE,
			.payload.window_close.window = WINDOWC_CRAFTING,
		};
		server_local_test_fail_next_entity_alloc();
		server_local_test_process_rpc(&s2, &close_crafting);
		assert(s2.player.active_inventory == lethal_window);
		struct server_rpc queued_drop = {
			.type = SRPC_DROP_ITEM,
			.payload.drop_item.all = false,
		};
		server_local_test_process_rpc(&s2, &queued_drop);
		assert(inventory_get_slot(&s2.player.inventory, INVENTORY_SLOT_HOTBAR,
								  &load_slot));
		assert(load_slot.id == hotbar_before_close_race.id
			   && load_slot.durability == hotbar_before_close_race.durability
			   && load_slot.count == hotbar_before_close_race.count);
		assert(count_item_entities(&s2, hotbar_before_close_race.id, 1) == 0);
		server_local_test_process_rpc(&s2, &close_crafting); // retry succeeds
		assert(s2.player.active_inventory == &s2.player.inventory);
		s2.player.finished_loading = finished_before_close_race;
		puts("  portal: failed lethal close rolls back health and i-frame state");
		puts("  portal: failed GUI close gates queued in-game item actions");
		const struct item_data portal_inventory_marker = {
			.id = ITEM_APPLE,
			.durability = 7,
			.count = 13,
		};
		inventory_set_slot(&s2.player.inventory, INVENTORY_SLOT_HOTBAR,
						   portal_inventory_marker);
		inventory_set_hotbar(&s2.player.inventory, 7);
		struct item_data expected_inventory[INVENTORY_SIZE];
		for(size_t i = 0; i < INVENTORY_SIZE; i++)
			expected_inventory[i] = s2.player.inventory.items[i];

		s2.player.x = 100.0;
		s2.player.y = 70.0;
		s2.player.z = -60.0;
		s2.player.fire = 123; // carried-state markers (Beta same-entity switch)
		s2.player.oxygen = 55;
		s2.player.vel_x = 0.375f;
		s2.player.vel_y = -0.5f;
		s2.player.old_vel_y = -0.25f;
		s2.player.vel_z = 0.625f;
		s2.player.fall_distance = 13.25F;
		s2.player.on_ground = true;
		s2.player.sneaking = true;
		s2.player.distance_walked = 3.75F;
		s2.player.next_step_distance = 6;
		int source_cooldown = s2.player.time_until_portal;
		float source_overlay = s2.player.time_in_portal;

		// Beta EntityPlayerSP closes any open GUI as soon as portal contact
		// begins. ContainerPlayer.onCraftGuiClosed drops the cursor and 2x2
		// crafting inputs into the source world instead of deleting or carrying
		// them across dimensions. Exercise that close even though this first
		// transaction deliberately aborts at its persistence gate.
		inventory_set_slot(
			&s2.player.inventory, INVENTORY_SLOT_OUTPUT,
			(struct item_data) {.id = ITEM_APPLE, .count = 1});
		inventory_set_slot(
			&s2.player.inventory, INVENTORY_SLOT_CRAFTING,
			(struct item_data) {.id = ITEM_APPLE, .durability = 2, .count = 2});
		inventory_set_picked_item(
			&s2.player.inventory,
			(struct item_data) {.id = ITEM_BREAD, .durability = 3, .count = 3});

		// The pause screen switches to world-select before UNLOAD reaches the
		// server. If close allocation fails, keep both worlds live and send the
		// cancellation RPC; never tear down only one side.
		server_local_test_fail_next_entity_alloc();
		server_local_unload_world_impl(&s2, true);
		struct item_data close_item;
		assert(s2.player.has_pos && s2.worlds[0].dimension == WORLD_DIM_OVERWORLD);
		assert(inventory_get_slot(&s2.player.inventory,
								  INVENTORY_SLOT_CRAFTING, &close_item));
		assert(inventory_get_picked_item(&s2.player.inventory, &close_item));
		puts("  portal: failed unload close keeps live world retryable");

		// The deferred Nether-death sequence must also stop after that failed
		// unload and re-arm itself; it must not call load over the live source.
		s2.death_world_evict = true;
		server_local_test_fail_next_entity_alloc();
		assert(!server_local_test_death_world_reload(&s2));
		assert(s2.death_world_evict && s2.player.has_pos
			   && s2.worlds[0].dimension == WORLD_DIM_OVERWORLD);
		s2.death_world_evict = false; // this test continues with live portal travel
		puts("  portal: failed death-world unload re-arms without double-load");

		// A close is part of the source transaction: if the Wii cannot allocate
		// an item entity, portal travel must abort with crafting/cursor stacks
		// still owned by the live inventory. Clearing first would durably delete
		// them at the following source checkpoint.
		uint32_t next_entity_before_close = s2.next_entity_id;
		server_local_test_fail_entity_alloc_after(2);
		server_local_portal_travel(&s2);
		assert(inventory_get_slot(&s2.player.inventory,
								  INVENTORY_SLOT_CRAFTING, &close_item));
		assert(close_item.id == ITEM_APPLE && close_item.durability == 2
			   && close_item.count == 2);
		assert(inventory_get_picked_item(&s2.player.inventory, &close_item));
		assert(close_item.id == ITEM_BREAD && close_item.durability == 3
			   && close_item.count == 3);
		assert(s2.next_entity_id == next_entity_before_close);
		assert(count_item_entities(&s2, ITEM_APPLE, 2) == 0);
		assert(count_item_entities(&s2, ITEM_BREAD, 3) == 0);
		assert(s2.player.dimension == WORLD_DIM_OVERWORLD
			   && s2.worlds[0].dimension == WORLD_DIM_OVERWORLD);
		assert(!s2.portal_job && !s2.portal_origin.valid);
		puts("  portal: failed close allocation aborts without item loss");

		// The source level checkpoint is a hard pre-teardown gate. Point its
		// output through the existing level.dat regular file (ENOTDIR) to force
		// a deterministic save failure, then prove the live world was untouched.
		string_t live_level_file;
		string_init_set(live_level_file, s2.level.file_name);
		string_set_str(s2.level.file_name,
					   test_fixture_join(TEST_PORTAL_TMP2, "/level.dat/nope"));
		struct world_switch_rpc_capture portal_abort_capture = {0};
		clin_test_set_rpc_observer(
			capture_world_switch_rpc, &portal_abort_capture);
		server_local_test_set_world_switch_handoff(
			capture_world_switch_handoff, &portal_abort_capture);
		server_local_portal_travel(&s2);
		server_local_test_set_world_switch_handoff(NULL, NULL);
		clin_test_set_rpc_observer(NULL, NULL);
		const enum client_rpc_type portal_abort_expected[] = {
			CRPC_RETURN_INGAME,
			CRPC_BEGIN_WORLD_SWITCH,
		};
		assert_world_switch_rpcs(
			&portal_abort_capture, portal_abort_expected,
			sizeof(portal_abort_expected) / sizeof(portal_abort_expected[0]));
		assert(portal_abort_capture.handoff_count == 1);
		assert(portal_abort_capture.handoff_phase
			   == SERVER_WORLD_SWITCH_PORTAL);
		assert(portal_abort_capture.player_pos_count == 1);
		assert(portal_abort_capture.has_input_handoff);
		assert(portal_abort_capture.input_epoch != 0);
		assert(!portal_abort_capture.input_handoff.payload.player_pos.fresh_body);
		assert(portal_abort_capture.input_handoff.payload.player_pos.selected_hotbar
			   == 7);
		assert(portal_abort_capture.input_handoff.payload.player_pos.world_time
			   == s2.world_time);
		assert(s2.world_switch_phase == SERVER_WORLD_SWITCH_PORTAL);
		assert(s2.input_ack_pending == portal_abort_capture.input_epoch);
		assert_world_switch_barrier_blocks(
			&s2, SERVER_WORLD_SWITCH_PORTAL);
		size_t abort_hotbar = inventory_get_hotbar(&s2.player.inventory);
		struct server_rpc stale_abort_hotbar = {
			.type = SRPC_HOTBAR_SLOT,
			.payload.hotbar_slot.slot
				= (uint8_t)((abort_hotbar + 1) % INVENTORY_SIZE_HOTBAR),
		};
		server_local_test_process_rpc(&s2, &stale_abort_hotbar);
		assert(inventory_get_hotbar(&s2.player.inventory) == abort_hotbar);
		struct server_rpc abort_ack = {
			.type = SRPC_INPUT_READY,
			.payload.input_ready.epoch = portal_abort_capture.input_epoch,
		};
		server_local_test_process_rpc(&s2, &abort_ack);
		assert(s2.input_ack_pending == 0);
		assert(s2.world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
		assert_source_rollback(&s2, source_cooldown, source_overlay);
		assert(!inventory_get_slot(
			&s2.player.inventory, INVENTORY_SLOT_OUTPUT, NULL));
		assert(!inventory_get_slot(
			&s2.player.inventory, INVENTORY_SLOT_CRAFTING, NULL));
		assert(!inventory_get_picked_item(&s2.player.inventory, NULL));
		assert_inventory_items_equal(&s2.player.inventory, expected_inventory);
		assert(inventory_get_hotbar(&s2.player.inventory) == 7);
		assert(count_item_entities(&s2, ITEM_APPLE, 2) == 1);
		assert(count_item_entities(&s2, ITEM_BREAD, 3) == 1);

		/* Save-and-quit uses the same hard checkpoint boundary. A level save
		 * failure must cancel the optimistic world-select transition with only
		 * RETURN_INGAME; WORLD_RESET before this point would destroy the sole
		 * client copy while the authoritative source remains live. */
		uint32_t unload_level_next_id = s2.next_entity_id;
		bool unload_level_finished = s2.player.finished_loading;
		struct world_switch_rpc_capture unload_level_capture = {0};
		clin_test_set_rpc_observer(
			capture_world_switch_rpc, &unload_level_capture);
		server_local_unload_world_impl(&s2, true);
		clin_test_set_rpc_observer(NULL, NULL);
		const enum client_rpc_type unload_abort_expected[] = {
			CRPC_RETURN_INGAME,
		};
		assert_world_switch_rpcs(
			&unload_level_capture, unload_abort_expected,
			sizeof(unload_abort_expected) / sizeof(unload_abort_expected[0]));
		assert(s2.player.has_pos
			   && s2.player.finished_loading == unload_level_finished);
		assert(s2.worlds[0].dimension == WORLD_DIM_OVERWORLD);
		assert(s2.world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
		assert(s2.next_entity_id == unload_level_next_id);
		assert(count_item_entities(&s2, ITEM_APPLE, 2) == 1);
		assert(count_item_entities(&s2, ITEM_BREAD, 3) == 1);
		string_set(s2.level.file_name, live_level_file);
		string_clear(live_level_file);

		/* The legacy flat tables are still authoritative for not-yet-migrated
		 * records. Their writes are part of the same hard boundary as chunk NBT:
		 * failing either snapshot retains every live source owner and emits no
		 * client reset. The prior close paths produced item entities in multiple
		 * chunks; make every owner resident so this reaches the intended legacy-write
		 * failure instead of correctly failing earlier at entity-owner preflight.
		 * Exercise chest-first and sign-second failures separately. */
		load_live_entity_owner_chunks(&s2);
		for(unsigned writes_before_failure = 0; writes_before_failure <= 1;
			writes_before_failure++) {
			complex_block_archive_test_fail_write_after(
				writes_before_failure);
			struct world_switch_rpc_capture legacy_capture = {0};
			clin_test_set_rpc_observer(
				capture_world_switch_rpc, &legacy_capture);
			server_local_portal_travel(&s2);
			clin_test_set_rpc_observer(NULL, NULL);
			assert_world_switch_rpcs(
				&legacy_capture, portal_abort_expected,
				sizeof(portal_abort_expected)
					/ sizeof(portal_abort_expected[0]));
			assert(s2.player.has_pos && !s2.portal_job
				   && !s2.portal_origin.valid);
			assert(count_item_entities(&s2, ITEM_APPLE, 2) == 1);
			assert(count_item_entities(&s2, ITEM_BREAD, 3) == 1);
			acknowledge_input_direct(&s2);
		}
		puts("  portal: chest/sign snapshot failures retain live source");

		/* Checkpoints are sparse: the portal's synchronous pre-teardown gate must
		 * not rewrite every resident chunk to FAT. Pin each reason that DOES require
		 * one rewrite, plus the untouched-empty skip and stale-list retry semantics. */
		struct server_chunk* sparse_sc = NULL;
		assert(server_world_load_chunk(&s2.worlds[0], 20, 20, &sparse_sc)
			   && sparse_sc);
		/* Establish a known empty disk record even when a prior interrupted test
		 * left this coordinate behind. */
		sparse_sc->modified = true;
		server_world_test_chunk_write_attempts_reset();
		assert(server_world_save_chunk(&s2.worlds[0], false, 20, 20));
		assert(server_world_test_chunk_write_attempts() == 1);
		assert(!sparse_sc->modified && !sparse_sc->persisted_entities_nonempty);

		server_world_test_chunk_write_attempts_reset();
		assert(server_world_save_chunk(&s2.worlds[0], false, 20, 20));
		assert(server_world_test_chunk_write_attempts() == 0);

		sparse_sc->modified = true;
		server_world_test_chunk_write_attempts_reset();
		assert(server_world_save_chunk(&s2.worlds[0], false, 20, 20));
		assert(server_world_test_chunk_write_attempts() == 1);
		assert(!sparse_sc->modified);

		int sparse_chest = -1;
		for(int i = 0; i < MAX_CHESTS; i++)
			if(s2.chest_pos[i].y < 0) {
				sparse_chest = i;
				break;
			}
		assert(sparse_chest >= 0);
		s2.chest_pos[sparse_chest]
			= (struct complex_block_pos) {321, 80, 321};
		s2.te_owners.chest_slot[sparse_chest] = s2.worlds[0].world_index; /* A1b */
		s2.te_owners.chest_gen[sparse_chest] = s2.worlds[0].generation;
		s2.chest_items[sparse_chest][0]
			= (struct item_data) {.id = ITEM_STICK, .count = 1};
		server_world_test_chunk_write_attempts_reset();
		assert(server_world_save_chunk(&s2.worlds[0], false, 20, 20));
		assert(server_world_test_chunk_write_attempts() == 1);
		assert(!sparse_sc->modified);
		/* Removing a tile entity also changes its block in production. Model that
		 * dirty block edge so the prior TileEntities list is explicitly cleared. */
		s2.chest_pos[sparse_chest].y = -1;
		sparse_sc->modified = true;
		server_world_test_chunk_write_attempts_reset();
		assert(server_world_save_chunk(&s2.worlds[0], false, 20, 20));
		assert(server_world_test_chunk_write_attempts() == 1);

		struct item_data sparse_item = {.id = ITEM_STICK, .count = 1};
		struct entity* sparse_entity = server_local_spawn_item(
			(vec3) {321.5f, 80.5f, 321.5f}, &sparse_item, false, &s2);
		assert(sparse_entity);
		uint32_t sparse_entity_id = sparse_entity->id;
		server_world_test_chunk_write_attempts_reset();
		assert(server_world_save_chunk(&s2.worlds[0], false, 20, 20));
		assert(server_world_test_chunk_write_attempts() == 1);
		assert(sparse_sc->persisted_entities_nonempty);

		server_local_test_fail_next_entity_gather();
		server_world_test_chunk_write_attempts_reset();
		assert(!server_world_save_chunk(&s2.worlds[0], false, 20, 20));
		assert(server_world_test_chunk_write_attempts() == 0);
		assert(sparse_sc->persisted_entities_nonempty);
		struct entity** sparse_entity_slot
			= dict_entity_get(s2.entities, sparse_entity_id);
		assert(sparse_entity_slot && *sparse_entity_slot == sparse_entity);
		free(*sparse_entity_slot);
		dict_entity_erase(s2.entities, sparse_entity_id);

		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
		server_world_test_chunk_write_attempts_reset();
		assert(!server_world_save_chunk(&s2.worlds[0], false, 20, 20));
		assert(server_world_test_chunk_write_attempts() == 1);
		assert(!sparse_sc->modified && sparse_sc->persisted_entities_nonempty);
		server_world_test_chunk_write_attempts_reset();
		assert(server_world_save_chunk(&s2.worlds[0], false, 20, 20));
		assert(server_world_test_chunk_write_attempts() == 1);
		assert(!sparse_sc->persisted_entities_nonempty);
		server_world_test_chunk_write_attempts_reset();
		assert(server_world_save_chunk(&s2.worlds[0], false, 20, 20));
		assert(server_world_test_chunk_write_attempts() == 0);
		puts("  checkpoint: sparse dirty/TE/entity/stale-clear writes + empty skip OK");

		// A region/entity checkpoint failure must abort while the only live source
		// world and its dirty chunk are still resident and retryable.
		struct server_chunk* checkpoint_sc = dict_server_chunks_get(
			s2.worlds[0].chunks, S_CHUNK_ID(6, -4));
		if(!checkpoint_sc)
			assert(server_world_load_chunk(
				&s2.worlds[0], 6, -4, &checkpoint_sc));
		assert(checkpoint_sc);
		assert(server_world_set_block(
			&s2.worlds[0], 103, 80, -60,
			(struct block_data) {.type = BLOCK_COBBLESTONE}));
		int checkpoint_chest = -1;
		for(int i = 0; i < MAX_CHESTS; i++)
			if(s2.chest_pos[i].y < 0) {
				checkpoint_chest = i;
				break;
			}
		assert(checkpoint_chest >= 0);
		s2.chest_pos[checkpoint_chest]
			= (struct complex_block_pos) {102, 80, -60};
		s2.te_owners.chest_slot[checkpoint_chest] = s2.worlds[0].world_index; /* A1b */
		s2.te_owners.chest_gen[checkpoint_chest] = s2.worlds[0].generation;
		s2.chest_items[checkpoint_chest][0]
			= (struct item_data) {.id = ITEM_APPLE, .count = 1};
		struct item_data checkpoint_item = {
			.id = ITEM_DIAMOND_PICKAXE, .count = 1,
		};
		struct entity* checkpoint_entity = server_local_spawn_item(
			(vec3) {103.5f, 80.5f, -60.5f}, &checkpoint_item, false, &s2);
		assert(checkpoint_entity);
		uint32_t checkpoint_entity_id = checkpoint_entity->id;
		struct entity* checkpoint_tnt = server_local_spawn_primed_tnt(
			&s2, (vec3) {104.5f, 80.5f, -60.5f}, 37);
		assert(checkpoint_tnt);
		uint32_t checkpoint_tnt_id = checkpoint_tnt->id;
		uint32_t checkpoint_next_id = s2.next_entity_id;
		bool checkpoint_finished_loading = s2.player.finished_loading;

		/* Exercise every fallible chunk-snapshot class through ordinary unload:
		 * archive write, entity gather allocation, and tile-entity gather
		 * allocation. All must retain the exact live pointers/ids and send only
		 * RETURN_INGAME, then remain retryable. */
		for(int failure = 0; failure < 3; failure++) {
			if(failure == 0)
				region_archive_test_fail_next(
					REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
			else if(failure == 1)
				server_local_test_fail_next_entity_gather();
			else
				te_store_test_fail_allocation_after(0);

			struct world_switch_rpc_capture unload_chunk_capture = {0};
			clin_test_set_rpc_observer(
				capture_world_switch_rpc, &unload_chunk_capture);
			server_local_unload_world_impl(&s2, true);
			clin_test_set_rpc_observer(NULL, NULL);
			assert_world_switch_rpcs(
				&unload_chunk_capture, unload_abort_expected,
				sizeof(unload_abort_expected)
					/ sizeof(unload_abort_expected[0]));
			assert(s2.player.has_pos
				   && s2.player.finished_loading
					  == checkpoint_finished_loading);
			assert(s2.worlds[0].dimension == WORLD_DIM_OVERWORLD);
			assert(s2.world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
			assert(server_world_is_chunk_loaded(&s2.worlds[0], 6, -4));
			assert(s2.chest_pos[checkpoint_chest].y == 80);
			struct entity** item_slot
				= dict_entity_get(s2.entities, checkpoint_entity_id);
			struct entity** tnt_slot
				= dict_entity_get(s2.entities, checkpoint_tnt_id);
			assert(item_slot && *item_slot == checkpoint_entity);
			assert(tnt_slot && *tnt_slot == checkpoint_tnt);
			assert(s2.next_entity_id == checkpoint_next_id);
		}
		puts("  unload: level/region/entity/TE failures retain live world");

		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
		server_local_portal_travel(&s2);
		acknowledge_input_direct(&s2);
		assert_source_rollback(&s2, source_cooldown, source_overlay);
		assert(server_world_is_chunk_loaded(&s2.worlds[0], 6, -4));
		assert(checkpoint_sc->modified);
		assert(s2.chest_pos[checkpoint_chest].y == 80);
		assert(dict_entity_get(s2.entities, checkpoint_entity_id));
		assert(dict_entity_get(s2.entities, checkpoint_tnt_id));

		// The erase path obeys the same commit boundary: failed persistence
		// retains the chunk and every live owner, so the caller can back off.
		//
		// G9 R1 (d8721d8cf, coherent checkpoints without early live drains):
		// the checkpoints above settle light only in a rolled-back trial, so
		// the fixture's edits leave pending light pinning this column, and a
		// pinned column is NEVER written -- the save is deferred
		// before any archive write. Pin that deferral explicitly (no write
		// attempted, chunk and owners retained), then settle the queue as
		// labelled setup so the injected fault below hits the archive write
		// itself. Without the settle the deferral returned false first, the
		// PAYLOAD_WRITE injection stayed armed, and it leaked into the
		// legacy-deletion travel below, which then aborted at its checkpoint
		// instead of consuming its destination-load injection (R-B03).
		assert(server_world_light_pinned(&s2.worlds[0], 6, -4));
		server_world_test_chunk_write_attempts_reset();
		assert(!server_world_save_chunk(&s2.worlds[0], true, 6, -4));
		assert(server_world_test_chunk_write_attempts() == 0);
		assert(server_world_is_chunk_loaded(&s2.worlds[0], 6, -4));
		assert(checkpoint_sc->modified);
		assert(s2.chest_pos[checkpoint_chest].y == 80);
		assert(server_world_light_drain(&s2.worlds[0])); /* settled setup */
		assert(!server_world_light_pinned(&s2.worlds[0], 6, -4));
		// STAGE22 (8837cf141) + SOL-REVIEW-d42f (bf92f3e33): an EVICTING save
		// of a chunk with live entities first needs client-inbox headroom for
		// its destroy batch, and tokens behind deferred output are not
		// headroom. The rollback cases above leave a deferred backlog larger
		// than the 16-token inbox; production retries next tick while the
		// client drains it. Let this fixture's consumer (drain_entry) catch up
		// so the save is admitted to the archive write (destroy batch +
		// UNLOAD = 2 tokens, server_local_evict_headroom_w).
		wait_client_consumer(2);
		server_world_test_chunk_write_attempts_reset();
		region_archive_test_fail_next(REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
		assert(!server_world_save_chunk(&s2.worlds[0], true, 6, -4));
		assert(server_world_test_chunk_write_attempts() == 1);
		assert(server_world_is_chunk_loaded(&s2.worlds[0], 6, -4));
		assert(s2.chest_pos[checkpoint_chest].y == 80);
		assert(dict_entity_get(s2.entities, checkpoint_entity_id));
		assert(dict_entity_get(s2.entities, checkpoint_tnt_id));

		// Tile-entity snapshot OOM is also a hard checkpoint failure. It must not
		// degrade to an empty list and commit away the live chest.
		te_store_test_fail_allocation_after(0);
		server_local_portal_travel(&s2);
		acknowledge_input_direct(&s2);
		assert_source_rollback(&s2, source_cooldown, source_overlay);
		assert(server_world_is_chunk_loaded(&s2.worlds[0], 6, -4));
		assert(s2.chest_pos[checkpoint_chest].y == 80);
		assert(dict_entity_get(s2.entities, checkpoint_entity_id));
		assert(dict_entity_get(s2.entities, checkpoint_tnt_id));

		struct entity** checkpoint_entity_slot
			= dict_entity_get(s2.entities, checkpoint_entity_id);
		assert(checkpoint_entity_slot && *checkpoint_entity_slot);
		free(*checkpoint_entity_slot);
		dict_entity_erase(s2.entities, checkpoint_entity_id);
		struct entity** checkpoint_tnt_slot
			= dict_entity_get(s2.entities, checkpoint_tnt_id);
		assert(checkpoint_tnt_slot && *checkpoint_tnt_slot);
		free(*checkpoint_tnt_slot);
		dict_entity_erase(s2.entities, checkpoint_tnt_id);
		s2.chest_pos[checkpoint_chest].y = -1;

		/* Entity NBT belongs to the entity's current chunk. Movement can cross the
		 * resident frontier between stream passes; a transition must not checkpoint
		 * only resident chunks and then free that off-resident sole owner. Use a
		 * negative boundary coordinate to pin floor/chunk arithmetic too. */
		struct item_data frontier_item = {.id = ITEM_STICK, .count = 1};
		struct entity* frontier_entity = server_local_spawn_item(
			(vec3) {103.5f, 80.5f, -60.5f}, &frontier_item, false, &s2);
		assert(frontier_entity);
		uint32_t frontier_entity_id = frontier_entity->id;
		frontier_entity->pos[0] = -1600.5f; /* floor(-1600.5 / 16) = -101 */
		frontier_entity->pos[2] = -1600.5f;
		assert(!server_world_is_chunk_loaded(&s2.worlds[0], -101, -101));
		struct world_switch_rpc_capture frontier_capture = {0};
		clin_test_set_rpc_observer(
			capture_world_switch_rpc, &frontier_capture);
		server_local_portal_travel(&s2);
		clin_test_set_rpc_observer(NULL, NULL);
		const enum client_rpc_type frontier_expected[] = {
			CRPC_RETURN_INGAME,
			CRPC_BEGIN_WORLD_SWITCH,
		};
		assert_world_switch_rpcs(
			&frontier_capture, frontier_expected,
			sizeof(frontier_expected) / sizeof(frontier_expected[0]));
		assert(s2.player.has_pos && !s2.portal_job
			   && !s2.portal_origin.valid);
		struct entity** frontier_slot
			= dict_entity_get(s2.entities, frontier_entity_id);
		assert(frontier_slot && *frontier_slot == frontier_entity);
		acknowledge_input_direct(&s2);
		free(*frontier_slot);
		dict_entity_erase(s2.entities, frontier_entity_id);
		puts("  portal: off-resident entity owner fails checkpoint closed");

		/* A full-image legacy snapshot carries deletions too. Seed committed rows,
		 * delete them in RAM, then force destination open to fail after the source
		 * checkpoint. Rollback reload must not resurrect either old row. */
		int deletion_sign = -1;
		for(int i = 0; i < MAX_SIGNS; i++)
			if(s2.sign_pos[i].y < 0) {
				deletion_sign = i;
				break;
			}
		assert(deletion_sign >= 0);
		s2.chest_pos[checkpoint_chest]
			= (struct complex_block_pos) {102, 80, -60};
		s2.te_owners.chest_slot[checkpoint_chest] = s2.worlds[0].world_index; /* A1b */
		s2.te_owners.chest_gen[checkpoint_chest] = s2.worlds[0].generation;
		s2.chest_items[checkpoint_chest][0]
			= (struct item_data) {.id = ITEM_APPLE, .count = 1};
		s2.sign_pos[deletion_sign]
			= (struct complex_block_pos) {105, 80, -60};
		s2.te_owners.sign_slot[deletion_sign] = s2.worlds[0].world_index; /* A1b */
		s2.te_owners.sign_gen[deletion_sign] = s2.worlds[0].generation;
		memset(s2.sign_texts[deletion_sign], ' ', SIGN_SIZE);
		memcpy(s2.sign_texts[deletion_sign], "DELETE", 6);
		assert(chest_archive_write(
			s2.chest_pos, s2.chest_items[0], s2.level_name,
			s2.worlds[0].dimension));
		assert(sign_archive_write(
			s2.sign_pos, s2.sign_texts[0], s2.level_name,
			s2.worlds[0].dimension));
		s2.chest_pos[checkpoint_chest].y = -1;
		s2.sign_pos[deletion_sign].y = -1;
		checkpoint_sc->modified = true;

		// A destination archive-open failure must reconstruct the still-committed
		// source session, including transient player state.
		server_local_test_fail_next_world_load();
		server_local_portal_travel(&s2);
		assert(!s2.portal_job);
		assert(s2.player.has_pos);
		assert(s2.player.dimension == WORLD_DIM_OVERWORLD);
		assert(s2.worlds[0].dimension == WORLD_DIM_OVERWORLD);
		assert(s2.player.x == 100.0 && s2.player.y == 70.0
			   && s2.player.z == -60.0);
		assert(s2.player.fire == 123 && s2.player.oxygen == 55);
		assert(s2.player.on_ground && s2.player.sneaking);
		assert(s2.chest_pos[checkpoint_chest].y < 0);
		assert(s2.sign_pos[deletion_sign].y < 0);
		assert_source_rollback(&s2, source_cooldown, source_overlay);
		finish_and_assert_portal_inventory(&s2, expected_inventory, 7);
		puts("  portal: legacy chest/sign deletion snapshot survives rollback");

		// A failure after the destination is live follows the same rollback path.
		server_local_portal_travel(&s2);
		assert(s2.portal_job);
		assert_world_switch_barrier_blocks(
			&s2, SERVER_WORLD_SWITCH_PORTAL);
		portal_teleporter_test_fail_next_step();
		assert(server_local_portal_travel_step(&s2));
		assert(!s2.portal_job);
		assert(s2.player.has_pos);
		assert(s2.player.dimension == WORLD_DIM_OVERWORLD);
		assert(s2.worlds[0].dimension == WORLD_DIM_OVERWORLD);
		assert(s2.player.x == 100.0 && s2.player.y == 70.0
			   && s2.player.z == -60.0);
		assert(s2.player.fire == 123 && s2.player.oxygen == 55);
		assert_source_rollback(&s2, source_cooldown, source_overlay);
		finish_and_assert_portal_inventory(&s2, expected_inventory, 7);

		// Destination load failure followed by a transient failure reopening the
		// SOURCE must retain origin/name for retry, not soft-lock has_pos=0.
		server_local_test_fail_world_loads(2);
		server_local_portal_travel(&s2);
		assert(!s2.portal_job);
		assert(!s2.player.has_pos && s2.portal_origin.valid);
		assert_world_switch_barrier_blocks(
			&s2, SERVER_WORLD_SWITCH_PORTAL);
		assert(finish_portal_travel(&s2) > 20);
		assert_source_rollback(&s2, source_cooldown, source_overlay);
		finish_and_assert_portal_inventory(&s2, expected_inventory, 7);

		/* A permanently unreadable source must not spin forever behind the loading
		 * screen. One destination failure plus five source-reopen failures reaches a
		 * one-shot terminal RPC while preserving the durable origin/name for restart
		 * or explicit recovery. */
		struct world_switch_rpc_capture terminal_capture = {0};
		clin_test_set_rpc_observer(
			capture_world_switch_rpc, &terminal_capture);
		server_local_test_fail_world_loads(6);
		server_local_portal_travel(&s2);
		for(int tick = 0; tick < 200 && !s2.load_failure_terminal; tick++)
			(void)server_local_portal_travel_step(&s2);
		assert(s2.load_failure_terminal);
		assert(!s2.player.has_pos && !s2.portal_job && s2.portal_origin.valid);
		assert(s2.portal_origin.rollback_retry_count == 5);
		assert(s2.portal_origin.rollback_retry_delay == 0);
		assert(terminal_capture.load_failed_count == 1);
		for(int tick = 0; tick < 50; tick++)
			assert(server_local_portal_travel_step(&s2));
		assert(terminal_capture.load_failed_count == 1);
		assert(s2.portal_origin.rollback_retry_count == 5);
		clin_test_set_rpc_observer(NULL, NULL);

		/* Continue this integration fixture through the same retained origin after
		 * proving the terminal stop. A process restart would perform this reopen. */
		s2.load_failure_terminal = false;
		assert(server_local_portal_travel_step(&s2));
		assert_source_rollback(&s2, source_cooldown, source_overlay);
		finish_and_assert_portal_inventory(&s2, expected_inventory, 7);
		puts("  portal: rollback retries cap at one terminal LOAD_FAILED");

		// Fail the second portal-frame write. Rollback must discard the first
		// successful destination mutation instead of saving a partial frame.
		assert(!destination_region_exists());
		portal_teleporter_test_fail_write_after(1);
		server_local_portal_travel(&s2);
		assert(s2.portal_job);
		int fresh_create_steps = finish_portal_travel(&s2);
		/* No destination portal exists here: this covers FIND + CREATE, not the
		 * later durable-portal FIND-only success. Reverting CREATE 8->2 columns
		 * would exceed this hardware-facing scheduling bound. */
		assert(fresh_create_steps > 1 && fresh_create_steps <= 512);
		assert(portal_teleporter_test_write_failure_fired());
		assert_source_rollback(&s2, source_cooldown, source_overlay);
		finish_and_assert_portal_inventory(&s2, expected_inventory, 7);
		assert(!destination_region_exists());

		/* Simulate power loss at the exact chunks-first boundary. The newly built
		 * portal must already be durable, while level.dat must still restart the
		 * player at the source coordinates. */
		unsigned player_commit_attempts
			= server_local_test_portal_player_commit_attempts();
		server_local_test_fail_next_portal_after_chunk_checkpoint();
		server_local_portal_travel(&s2);
		assert(s2.portal_job);
		assert(finish_portal_travel(&s2) > 1);
		assert_source_rollback(&s2, source_cooldown, source_overlay);
		finish_and_assert_portal_inventory(&s2, expected_inventory, 7);
		assert_saved_player_is_source(TEST_PORTAL_TMP2);
		assert(server_local_test_portal_player_commit_attempts()
			   == player_commit_attempts);
		assert(destination_region_exists());
		assert(count_saved_destination_portal_blocks() == 6);
		puts("  portal: power loss after chunk checkpoint leaves portal durable and player at source");
		clear_destination_regions();

		/* F1 (audit 2026-08-04): the region fsync barrier must GATE the Player
		 * commit. Before the fix, level.dat could durably point the player into
		 * a dimension whose chunks were still cache-only. A refused sync must
		 * roll back with the Player write never attempted. FAIL-BEFORE: without
		 * the barrier the armed failure is never consulted and the commit
		 * counter advances. */
		player_commit_attempts
			= server_local_test_portal_player_commit_attempts();
		server_local_test_fail_next_region_sync();
		server_local_portal_travel(&s2);
		assert(s2.portal_job);
		assert(finish_portal_travel(&s2) > 1);
		assert_source_rollback(&s2, source_cooldown, source_overlay);
		finish_and_assert_portal_inventory(&s2, expected_inventory, 7);
		assert_saved_player_is_source(TEST_PORTAL_TMP2);
		assert(server_local_test_portal_player_commit_attempts()
			   == player_commit_attempts);
		puts("  portal: region-sync failure rolls back before the player commit");
		clear_destination_regions();

		/* A failed destination chunk write must stop before the Player tag can
		 * commit. Some earlier chunks may have checkpointed; level.dat must still
		 * select the source world on restart. */
		server_local_portal_travel(&s2);
		assert(s2.portal_job);
		region_archive_test_fail_next(
			REGION_ARCHIVE_TEST_FAIL_PAYLOAD_WRITE);
		assert(finish_portal_travel(&s2) > 1);
		assert_source_rollback(&s2, source_cooldown, source_overlay);
		finish_and_assert_portal_inventory(&s2, expected_inventory, 7);
		assert_saved_player_is_source(TEST_PORTAL_TMP2);
		assert(server_local_test_portal_player_commit_attempts()
			   == player_commit_attempts);
		assert(destination_region_exists());
		puts("  portal: destination chunk-save failure cannot commit player");
		clear_destination_regions();

		/* Destination chunks are the atomic owner image and must commit before the
		 * legacy migration snapshot. A legacy failure can therefore leave marked
		 * chunks durable, but still may not commit the Player tag. */
		unsigned legacy_commit_attempts
			= server_local_test_portal_player_commit_attempts();
		server_local_portal_travel(&s2);
		assert(s2.portal_job);
		server_world_test_chunk_write_attempts_reset();
		complex_block_archive_test_fail_write_after(0);
		assert(finish_portal_travel(&s2) > 1);
		assert_source_rollback(&s2, source_cooldown, source_overlay);
		finish_and_assert_portal_inventory(&s2, expected_inventory, 7);
		assert(server_world_test_chunk_write_attempts() > 0);
		assert(server_local_test_portal_player_commit_attempts()
			   == legacy_commit_attempts);
		puts("  portal: destination legacy failure follows chunks and precedes player");
		clear_destination_regions();

		// A completed frame followed by a real level.dat write failure also rolls
		// back the live player, while retaining the already-durable portal chunks.
		server_local_portal_travel(&s2);
		assert(s2.portal_job);
		string_set_str(s2.level.file_name,
					   test_fixture_join(TEST_PORTAL_TMP2, "/level.dat/nope"));
		assert(finish_portal_travel(&s2) > 1);
		assert_source_rollback(&s2, source_cooldown, source_overlay);
		finish_and_assert_portal_inventory(&s2, expected_inventory, 7);
		assert_saved_player_is_source(TEST_PORTAL_TMP2);
		assert(server_local_test_portal_player_commit_attempts()
			   == player_commit_attempts + 1);
		assert(count_saved_destination_portal_blocks() == 6);

		// Teardown must run an open container's commit/cleanup hook before chunk
		// and entity persistence gather the source world.
		struct inventory* test_container = calloc(1, sizeof(*test_container));
		assert(test_container);
		assert(inventory_create(test_container, &test_container_logic, NULL,
								&s2.player, 1, 0, 0, 0));
		test_container_close_count = 0;
		s2.player.active_inventory = test_container;
		/* Put a stateful drop in the departure chunk. The real portal unload must
		 * save it before clearing the dictionary, and the return-side chunk load
		 * must reconstruct it. Clear any residue from an interrupted prior test
		 * run before installing the marker. */
		struct server_chunk* entity_sc = NULL;
		assert(server_world_load_chunk(&s2.worlds[0], 6, -4, &entity_sc) && entity_sc);
		{
			dict_entity_it_t eit;
			dict_entity_it(eit, s2.entities);
			while(!dict_entity_end_p(eit)) {
				free(dict_entity_ref(eit)->value);
				dict_entity_next(eit);
			}
			dict_entity_reset(s2.entities);
			s2.next_entity_id = 0;
		}
		struct item_data portal_item = {
			.id = ITEM_DIAMOND_PICKAXE, .durability = 1234, .count = 1,
		};
		struct entity* carried_drop = server_local_spawn_item(
			(vec3) {100.25f, 70.5f, -60.25f}, &portal_item, false, &s2);
		assert(carried_drop);
		carried_drop->data.item.age = 321;
		glm_vec3_copy((vec3) {0.125f, -0.25f, 0.375f}, carried_drop->vel);
		struct world_switch_rpc_capture portal_success_capture = {0};
		clin_test_set_rpc_observer(
			capture_world_switch_rpc, &portal_success_capture);
		/* Earlier rollback cases reopen only the explicitly requested source chunk.
		 * Materialize a clean resident set so this assertion proves the checkpoint
		 * cost is sparse in resident count, not merely one write for one chunk. */
		for(w_coord_t cx = 20; cx < 28; cx++) {
			struct server_chunk* latency_sc = NULL;
			if(server_world_is_chunk_loaded(&s2.worlds[0], cx, 20))
				latency_sc = dict_server_chunks_get(
					s2.worlds[0].chunks, S_CHUNK_ID(cx, 20));
			else
				assert(server_world_load_chunk(
						   &s2.worlds[0], cx, 20, &latency_sc)
					   && latency_sc);
			latency_sc->modified = true;
			assert(server_world_save_chunk(&s2.worlds[0], false, cx, 20));
			assert(!latency_sc->modified
				   && !latency_sc->persisted_entities_nonempty);
		}
		size_t source_resident_chunks = dict_server_chunks_size(s2.worlds[0].chunks);
		assert(source_resident_chunks >= 9);
		server_world_test_chunk_write_attempts_reset();
		server_local_portal_travel(&s2);
		/* The departure checkpoint has one live entity chunk. Resident empty chunks
		 * are inspected but do not become synchronous SD writes. This deterministic
		 * write-count is the real-Wii latency proxy for the reported loading hang. */
		assert(server_world_test_chunk_write_attempts() == 1);
		assert(test_container_close_count == 1);
		assert(s2.player.active_inventory == &s2.player.inventory);

		/* Destination fallback/build edits run behind the loading barrier. Model
		 * the two durable side effects they can leave just before commit: an
		 * absent-host chest row retained after an EntityItem allocation failure,
		 * and a live sign deletion whose legacy file still contains the old row.
		 * Commit must resolve the item owner into one marked chunk image before
		 * shrinking the legacy files or committing the Player tag. */
		struct server_chunk* orphan_sc = NULL;
		assert(server_world_load_chunk(&s2.worlds[0], 30, 30, &orphan_sc)
			   && orphan_sc);
		const w_coord_t orphan_x = 30 * CHUNK_SIZE + 1;
		const w_coord_t orphan_y = 80;
		const w_coord_t orphan_z = 30 * CHUNK_SIZE + 1;
		struct block_data orphan_block;
		assert(server_world_get_block(
			   &s2.worlds[0], orphan_x, orphan_y, orphan_z, &orphan_block)
			   && orphan_block.type == BLOCK_AIR);
		int orphan_chest = -1, deleted_sign = -1;
		for(int i = 0; i < MAX_CHESTS && orphan_chest < 0; i++)
			if(s2.chest_pos[i].y < 0)
				orphan_chest = i;
		for(int i = 0; i < MAX_SIGNS && deleted_sign < 0; i++)
			if(s2.sign_pos[i].y < 0)
				deleted_sign = i;
		assert(orphan_chest >= 0 && deleted_sign >= 0);
		s2.chest_pos[orphan_chest]
			= (struct complex_block_pos) {orphan_x, orphan_y, orphan_z};
		s2.te_owners.chest_slot[orphan_chest] = s2.worlds[0].world_index; /* A1b */
		s2.te_owners.chest_gen[orphan_chest] = s2.worlds[0].generation;
		s2.chest_items[orphan_chest][0] = (struct item_data) {
			.id = ITEM_APPLE, .durability = 99, .count = 1,
		};
		s2.sign_pos[deleted_sign]
			= (struct complex_block_pos) {orphan_x + 1, orphan_y, orphan_z};
		s2.te_owners.sign_slot[deleted_sign] = s2.worlds[0].world_index; /* A1b */
		s2.te_owners.sign_gen[deleted_sign] = s2.worlds[0].generation;
		memset(s2.sign_texts[deleted_sign], ' ', SIGN_SIZE);
		memcpy(s2.sign_texts[deleted_sign], "STALE", 5);
		assert(chest_archive_write(
			s2.chest_pos, s2.chest_items[0], s2.level_name,
			WORLD_DIM_NETHER));
		assert(sign_archive_write(
			s2.sign_pos, s2.sign_texts[0], s2.level_name,
			WORLD_DIM_NETHER));
		s2.sign_pos[deleted_sign].y = -1; /* live deletion, stale disk row */

		/* Fail the sign snapshot after the chest deletion has already published.
		 * The orphan's EntityItem must survive the rollback boundary: a second
		 * destination load must recover exactly one apple, never zero or two. This
		 * also models power loss between cross-file owner commits. */
		complex_block_archive_test_fail_write_after(1);
		assert(finish_portal_travel(&s2) > 1);
		assert_source_rollback(&s2, source_cooldown, source_overlay);
		finish_and_assert_portal_inventory(&s2, expected_inventory, 7);
		memset(&portal_success_capture, 0, sizeof(portal_success_capture));
		/* The portal checkpoint is a production level.dat save/reopen seam. Keep
		 * Beta's optional Player.Spawn triple distinct from world SpawnXYZ across
		 * both dimension loads, rather than proving only the NBT helper in isolation. */
		const int32_t saved_bed_x = 14, saved_bed_y = 71, saved_bed_z = -8;
		s2.player.has_bed_spawn = true;
		s2.player.bed_spawn_x = saved_bed_x;
		s2.player.bed_spawn_y = saved_bed_y;
		s2.player.bed_spawn_z = saved_bed_z;
		server_local_portal_travel(&s2);
		assert(s2.portal_job);

		int outward_steps = finish_portal_travel(&s2);
		struct server_chunk* recovered_orphan_sc = NULL;
		assert(server_world_load_chunk(
			   &s2.worlds[0], 30, 30, &recovered_orphan_sc)
			   && recovered_orphan_sc);
		assert(s2.chest_pos[orphan_chest].y < 0);
		assert(count_item_entities_exact(&s2, ITEM_APPLE, 99, 1) == 1);
		for(int i = 0; i < MAX_SIGNS; i++)
			assert(s2.sign_pos[i].x != orphan_x + 1
				   || s2.sign_pos[i].y != orphan_y
				   || s2.sign_pos[i].z != orphan_z);
		static struct complex_block_pos durable_chest_pos[MAX_CHESTS];
		static struct item_data durable_chest_items[MAX_CHESTS][MAX_CHEST_SLOTS];
		static struct complex_block_pos durable_sign_pos[MAX_SIGNS];
		static char durable_sign_text[MAX_SIGNS][SIGN_SIZE];
		chest_archive_read(
			durable_chest_pos, durable_chest_items[0], s2.level_name,
			WORLD_DIM_NETHER);
		sign_archive_read(
			durable_sign_pos, durable_sign_text[0], s2.level_name,
			WORLD_DIM_NETHER);
		for(int i = 0; i < MAX_CHESTS; i++)
			assert(durable_chest_pos[i].x != orphan_x
				   || durable_chest_pos[i].y != orphan_y
				   || durable_chest_pos[i].z != orphan_z);
		int outward_load_ticks
			= finish_real_world_load(&s2, &portal_success_capture);
		clin_test_set_rpc_observer(NULL, NULL);
		const enum client_rpc_type portal_success_expected[] = {
			CRPC_RETURN_INGAME,
			CRPC_BEGIN_WORLD_SWITCH,
			CRPC_WORLD_RESET,
		};
		assert_world_switch_rpcs(
			&portal_success_capture, portal_success_expected,
			sizeof(portal_success_expected) / sizeof(portal_success_expected[0]));
		assert(outward_steps > 1 && outward_steps <= 512);
		assert(outward_load_ticks > 0 && portal_success_capture.chunk_count > 0
		   && portal_success_capture.player_pos_count == 1);
		assert(s2.player.dimension == WORLD_DIM_NETHER);
		assert(s2.worlds[0].dimension == WORLD_DIM_NETHER);
		assert(s2.player.has_bed_spawn
		   && s2.player.bed_spawn_x == saved_bed_x
		   && s2.player.bed_spawn_y == saved_bed_y
		   && s2.player.bed_spawn_z == saved_bed_z);
		assert(s2.worlds[0].populate_scratch_storage == session_populate_scratch);
		assert_inventory_items_equal(&s2.player.inventory, expected_inventory);
		assert(inventory_get_hotbar(&s2.player.inventory) == 7);
		// x/z scaled by exactly 8 (100/8=12.5, -60/8=-7.5) BEFORE the
		// Teleporter moved the player; arrival stays within its 16-block
		// create radius (+1 slack for the centering shifts).
		assert(fabs(s2.player.x - 12.5) <= 17.0);
		assert(fabs(s2.player.z - (-7.5)) <= 17.0);
		assert(s2.player.fire == 123 && s2.player.oxygen == 55);
		assert(s2.player.on_ground && s2.player.sneaking);
		assert(portal_success_capture.input_handoff.payload.player_pos.on_ground);
		assert(portal_success_capture.input_handoff.payload.player_pos.sneaking);
		assert(s2.player.time_until_portal == 10); // Beta arrival cooldown
		assert(s2.player.time_in_portal == 1.0f);
		// placed inside a freshly built nether portal
		{
			int px = (int)floor(s2.player.x), pz = (int)floor(s2.player.z);
			int py = (int)floor(s2.player.y - PLAYER_EYE_HEIGHT + 0.25);
			struct block_data pb;
			assert(server_world_get_block(&s2.worlds[0], px, py, pz, &pb)
				   && pb.type == BLOCK_PORTAL);
		}

		// Trip back: Beta-correct return = the NETHER position x8, not the
		// original departure point.
		double nx = s2.player.x, nz = s2.player.z;
		struct world_switch_rpc_capture return_capture = {0};
		clin_test_set_rpc_observer(capture_world_switch_rpc, &return_capture);
		server_local_portal_travel(&s2);
		int return_steps = finish_portal_travel(&s2);
		int return_load_ticks = finish_real_world_load(&s2, &return_capture);
		clin_test_set_rpc_observer(NULL, NULL);
		assert_world_switch_rpcs(
			&return_capture, portal_success_expected,
			sizeof(portal_success_expected) / sizeof(portal_success_expected[0]));
		assert(return_steps > 1 && return_steps <= 512);
		assert(return_load_ticks > 0 && return_capture.chunk_count > 0
		   && return_capture.player_pos_count == 1);
		assert(s2.player.dimension == WORLD_DIM_OVERWORLD);
		assert(s2.worlds[0].dimension == WORLD_DIM_OVERWORLD);
		assert(s2.player.has_bed_spawn
		   && s2.player.bed_spawn_x == saved_bed_x
		   && s2.player.bed_spawn_y == saved_bed_y
		   && s2.player.bed_spawn_z == saved_bed_z);
		assert(s2.worlds[0].populate_scratch_storage == session_populate_scratch);
		assert_inventory_items_equal(&s2.player.inventory, expected_inventory);
		assert(inventory_get_hotbar(&s2.player.inventory) == 7);
		assert(fabs(s2.player.x - nx * 8.0) <= 17.0);
		assert(fabs(s2.player.z - nz * 8.0) <= 17.0);
		/* Loading the marked far chunk suppressed its stale sign row in RAM. The
		 * synchronous Nether departure checkpoint now shrinks both compatibility
		 * archives only after that complete chunk image was durable. */
		chest_archive_read(
			durable_chest_pos, durable_chest_items[0], s2.level_name,
			WORLD_DIM_NETHER);
		sign_archive_read(
			durable_sign_pos, durable_sign_text[0], s2.level_name,
			WORLD_DIM_NETHER);
		for(int i = 0; i < MAX_CHESTS; i++)
			assert(durable_chest_pos[i].x != orphan_x
				   || durable_chest_pos[i].y != orphan_y
				   || durable_chest_pos[i].z != orphan_z);
		for(int i = 0; i < MAX_SIGNS; i++)
			assert(durable_sign_pos[i].x != orphan_x + 1
				   || durable_sign_pos[i].y != orphan_y
				   || durable_sign_pos[i].z != orphan_z);
		if(!server_world_is_chunk_loaded(&s2.worlds[0], 6, -4))
			assert(server_world_load_chunk(&s2.worlds[0], 6, -4, &entity_sc)
				   && entity_sc);
		int restored_drop_count = 0;
		dict_entity_it_t restored_it;
		dict_entity_it(restored_it, s2.entities);
		while(!dict_entity_end_p(restored_it)) {
			struct entity* e = dict_entity_ref(restored_it)->value;
			if(e && e->type == ENTITY_ITEM
			   && e->data.item.item.id == ITEM_DIAMOND_PICKAXE
			   && e->data.item.item.durability == 1234) {
				restored_drop_count++;
				assert(e->data.item.age == 321);
				assert(fabsf(e->vel[0] - 0.125f) < 1.0e-5f);
				assert(fabsf(e->vel[1] - (-0.25f)) < 1.0e-5f);
				assert(fabsf(e->vel[2] - 0.375f) < 1.0e-5f);
			}
			dict_entity_next(restored_it);
		}
		assert(restored_drop_count == 1);
		{
			int px = (int)floor(s2.player.x), pz = (int)floor(s2.player.z);
			int py = (int)floor(s2.player.y - PLAYER_EYE_HEIGHT + 0.25);
			struct block_data pb;
			assert(server_world_get_block(&s2.worlds[0], px, py, pz, &pb)
				   && pb.type == BLOCK_PORTAL);
		}

		// Exercise the exact hardware failure pattern beyond a single round trip:
		// Overworld -> Nether -> Overworld again, each through the production near
		// stream and actual finished_loading/PLAYER_POS gate. Return to Overworld
		// so the death-transaction cases below retain their original precondition.
		/* Clearing an invalid/removed bed must be just as durable. The next two
		 * production checkpoints remove the optional tags and reload them absent. */
		s2.player.has_bed_spawn = false;
		for(int repeat_leg = 0; repeat_leg < 2; repeat_leg++) {
			float walk_marker = 7.25F + (float)repeat_leg;
			int step_marker = 9 + repeat_leg;
			s2.player.distance_walked = walk_marker;
			s2.player.next_step_distance = step_marker;
			struct world_switch_rpc_capture repeat_capture = {0};
			clin_test_set_rpc_observer(
				capture_world_switch_rpc, &repeat_capture);
			server_local_portal_travel(&s2);
			assert(finish_portal_travel(&s2) > 1);
			assert(finish_real_world_load(&s2, &repeat_capture) > 0);
			clin_test_set_rpc_observer(NULL, NULL);
			assert_world_switch_rpcs(
				&repeat_capture, portal_success_expected,
				sizeof(portal_success_expected)
					/ sizeof(portal_success_expected[0]));
			assert(repeat_capture.chunk_count > 0
			   && repeat_capture.player_pos_count == 1);
			assert_inventory_items_equal(
				&s2.player.inventory, expected_inventory);
			assert(inventory_get_hotbar(&s2.player.inventory) == 7);
			assert(s2.player.distance_walked == walk_marker);
			assert(s2.player.next_step_distance == step_marker);
			enum world_dim expected_dim = repeat_leg == 0
				? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD;
			assert(s2.player.dimension == expected_dim
			   && s2.worlds[0].dimension == expected_dim);
			assert(!s2.player.has_bed_spawn);
			assert(s2.worlds[0].populate_scratch_storage
				   == session_populate_scratch);
			assert(!s2.worlds[0].populate_scratch_owned);
		}
		puts("  portal: four real streamed legs complete through PLAYER_POS gate");

		// A Nether-death reload can finish unloading and then fail to reopen the
		// archive (transient OOM/I/O). Preserve the owned world name, re-arm the
		// positionless retry, and skip a duplicate unload on the next attempt.
		string_t death_retry_name;
		string_init_set(death_retry_name, s2.level_name);
		string_t death_live_file;
		string_init_set(death_live_file, s2.level.file_name);
		string_set_str(s2.level.file_name,
					   test_fixture_join(TEST_PORTAL_TMP2, "/level.dat/nope"));
		s2.death_world_evict = false;
		s2.death_world_retry_delay = 0;
		assert(!server_local_test_death_world_reload(&s2));
		assert(s2.player.has_pos && s2.death_world_evict
		   && s2.death_world_retry_delay > 0);
		assert(s2.worlds[0].dimension == WORLD_DIM_OVERWORLD);
		string_set(s2.level.file_name, death_live_file);
		string_clear(death_live_file);
		s2.death_world_evict = false;
		s2.death_world_retry_delay = 0;

		server_local_test_fail_next_world_load();
		assert(!server_local_test_death_world_reload(&s2));
		assert(!s2.player.has_pos && s2.death_world_evict
		   && s2.death_world_retry_delay > 0);
		assert(strcmp(string_get_cstr(s2.level_name),
					  string_get_cstr(death_retry_name)) == 0);
		assert(!s2.inventory_finish_pending);
		s2.death_world_evict = false; // simulate the positionless retry dispatch
		s2.death_world_retry_delay = 0; // simulate expiry of the bounded backoff
		assert(server_local_test_death_world_reload(&s2));
		assert(s2.player.has_pos && s2.worlds[0].dimension == WORLD_DIM_OVERWORLD
		   && s2.player.dimension == WORLD_DIM_OVERWORLD);
		assert(s2.inventory_finish_pending);
		server_local_test_finish_load_inventory(&s2);
		assert(inventory_get_slot(&s2.player.inventory, INVENTORY_SLOT_HOTBAR,
								  &load_slot));
		/* debug.give kit sentinel: hotbar slot 0 is FLINT & STEEL since the
		   2026-07-28 full-inventory kit (was BLOCK_CHEST x64). */
		assert(load_slot.id == ITEM_FLINT_STEEL && load_slot.count == 1);
		string_clear(death_retry_name);
		puts("  portal: death checkpoint/load failures retain live/name and back off");

		// End-to-end BlockBed -> explosion damage -> Nether-death reload. A bed
		// itself NEVER changes dimension: a far blast must leave the live Nether
		// session untouched, while a close lethal blast latches the checked death
		// transaction and that transaction alone reopens the Overworld at spawn.
		server_local_portal_travel(&s2);
		assert(finish_portal_travel(&s2) > 1);
		assert(s2.player.dimension == WORLD_DIM_NETHER
			   && s2.worlds[0].dimension == WORLD_DIM_NETHER);
		server_local_test_finish_load_inventory(&s2);
		server_local_test_world_switch_player_pos_queued(&s2);
		s2.player.health = MAX_PLAYER_HEALTH;
		s2.player.hurt_resist_time = 0;
		s2.player.last_damage = 0;
		s2.death_world_evict = false;
		int bed_y = (int)floor(s2.player.y - PLAYER_EYE_HEIGHT + 0.01);
		w_coord_t bed_cx = (w_coord_t)floor(s2.player.x / CHUNK_SIZE);
		w_coord_t bed_cz = (w_coord_t)floor(s2.player.z / CHUNK_SIZE);
		assert(server_world_is_chunk_loaded(&s2.worlds[0], bed_cx, bed_cz));
		int bed_x = bed_cx * CHUNK_SIZE + CHUNK_SIZE / 2;
		int bed_z = bed_cz * CHUNK_SIZE + CHUNK_SIZE / 2;
		assert(server_world_set_block(
			&s2.worlds[0], bed_x, bed_y, bed_z,
			(struct block_data) {.type = BLOCK_BED, .metadata = 0}));
		assert(server_world_set_block(
			&s2.worlds[0], bed_x, bed_y, bed_z + 1,
			(struct block_data) {.type = BLOCK_BED, .metadata = 8}));
		struct block_data bed_foot = {.type = BLOCK_BED, .metadata = 0};
		struct block_info bed_info = {
			.block = &bed_foot, .x = bed_x, .y = bed_y, .z = bed_z,
		};
		// A stale/forged block RPC outside the authoritative reach radius must not
		// detonate the Nether bed or begin a world switch.
		s2.player.x = bed_x + 12.5;
		s2.player.y = bed_y + PLAYER_EYE_HEIGHT;
		s2.player.z = bed_z + 2.5;
		s2.player.fall_distance = 0.0F;
		struct world_switch_rpc_capture nonlethal_bed_capture = {0};
		clin_test_set_rpc_observer(
			capture_world_switch_rpc, &nonlethal_bed_capture);
		blocks[BLOCK_BED]->onRightClick(&s2, &s2.player, NULL, NULL, &bed_info, SIDE_TOP);
		clin_test_set_rpc_observer(NULL, NULL);
		assert_world_switch_rpcs(&nonlethal_bed_capture, NULL, 0);
		assert(s2.player.health == MAX_PLAYER_HEALTH);
		assert(!s2.death_world_evict);
		assert(s2.player.dimension == WORLD_DIM_NETHER
			   && s2.worlds[0].dimension == WORLD_DIM_NETHER);
		struct block_data remote_foot, remote_head;
		assert(server_world_get_block(
			&s2.worlds[0], bed_x, bed_y, bed_z, &remote_foot)
			   && remote_foot.type == BLOCK_BED);
		assert(server_world_get_block(
			&s2.worlds[0], bed_x, bed_y, bed_z + 1, &remote_head)
			   && remote_head.type == BLOCK_BED);

		// Move onto the faithful one-cell-past-head explosion center. Full-health
		// damage is unambiguously lethal.
		s2.player.x = bed_x + 0.5;
		s2.player.y = bed_y + PLAYER_EYE_HEIGHT;
		s2.player.z = bed_z + 2.5;
		s2.player.fall_distance = 0.0F;
		struct world_switch_rpc_capture lethal_bed_capture = {0};
		clin_test_set_rpc_observer(
			capture_world_switch_rpc, &lethal_bed_capture);
		server_local_test_set_world_switch_handoff(
			capture_world_switch_handoff, &lethal_bed_capture);
		blocks[BLOCK_BED]->onRightClick(&s2, &s2.player, NULL, NULL, &bed_info, SIDE_TOP);
		/* Deviation-#18 split: the bed blast commits only the DEAD state;
		 * the eviction/switch belongs to the explicit respawn (the handoff
		 * hook stays armed through it). */
		assert(s2.player.health <= 0);
		assert(!s2.death_world_evict);
		/* A prior sub-test's loading epoch is still armed; settle it -- the
		 * respawn gate requires a settled player like every ordinary action. */
		s2.player.finished_loading = true;
		assert(server_local_respawn_request(&s2));
		server_local_test_set_world_switch_handoff(NULL, NULL);
		assert(s2.death_world_evict);
		assert(s2.world_switch_phase == SERVER_WORLD_SWITCH_DEATH);
		assert(lethal_bed_capture.handoff_count == 1);
		assert(lethal_bed_capture.handoff_phase
			   == SERVER_WORLD_SWITCH_DEATH);
		assert(s2.player.health == MAX_PLAYER_HEALTH);
		assert(s2.player.dimension == WORLD_DIM_OVERWORLD
			   && s2.worlds[0].dimension == WORLD_DIM_NETHER);

		// A failed death checkpoint retains the phase barrier across its retry
		// delay. No stale pause/lifecycle/gameplay RPC can expose or replace the
		// split Nether-world/Overworld-player state.
		string_t lethal_live_file;
		string_init_set(lethal_live_file, s2.level.file_name);
		string_set_str(s2.level.file_name,
					   test_fixture_join(TEST_PORTAL_TMP2, "/level.dat/nope"));
		s2.death_world_evict = false; // simulate tick-top latch consumption
		assert(!server_local_test_death_world_reload(&s2));
		assert(s2.death_world_evict && s2.death_world_retry_delay > 0);
		assert_world_switch_barrier_blocks(
			&s2, SERVER_WORLD_SWITCH_DEATH);
		string_set(s2.level.file_name, lethal_live_file);
		string_clear(lethal_live_file);
		s2.death_world_evict = false;
		s2.death_world_retry_delay = 0;
		assert(server_local_test_death_world_reload(&s2));
		clin_test_set_rpc_observer(NULL, NULL);
		/* The barrier probe above injects a stale SRPC_UNLOAD_WORLD. Since
		 * 0db9e79ea (closing the silent barrier drop e11065a9d exposed with
		 * split_portal_quit_4m) the refused unload is ANSWERED with
		 * RETURN_INGAME so a pending Save and quit settles; the client keeps
		 * the world-switch barrier through it (client_interface.c
		 * CRPC_RETURN_INGAME: world_switch_pending reveals nothing). It is
		 * FIFO-between BEGIN and WORLD_RESET, never a second BEGIN/RESET. */
		const enum client_rpc_type lethal_bed_expected[] = {
			CRPC_RETURN_INGAME,
			CRPC_BEGIN_WORLD_SWITCH,
			CRPC_RETURN_INGAME,
			CRPC_WORLD_RESET,
		};
		assert_world_switch_rpcs(
			&lethal_bed_capture, lethal_bed_expected,
			sizeof(lethal_bed_expected) / sizeof(lethal_bed_expected[0]));
		assert(s2.player.has_pos && s2.worlds[0].dimension == WORLD_DIM_OVERWORLD
			   && s2.player.dimension == WORLD_DIM_OVERWORLD);
		server_local_test_finish_load_inventory(&s2);
		server_local_test_world_switch_player_pos_queued(&s2);
		assert(s2.world_switch_phase == SERVER_WORLD_SWITCH_IDLE);
		puts("  bed: nonlethal blast stays Nether; lethal blast reloads Overworld");

		// A normal load (the same public path used by Nether-death reload) must
		// still apply debug.give. Only live portal destination/rollback loads
		// preserve the same player's exact hotbar.
		s2.player.sleeping = true;
		s2.player.sleep_timer = 42;
		s2.player.sleep_x = 123;
		s2.player.sleep_y = 45;
		s2.player.sleep_z = -678;
		/* Find a real resident boundary and prove Beta's radius-32 activation
		 * gate keeps an ordinary durable entity attached before the complete
		 * production Save-and-quit checkpoint. */
		if(dict_server_chunks_size(s2.worlds[0].chunks) == 0) {
			struct server_chunk* player_chunk = NULL;
			w_coord_t player_cx
				= (w_coord_t)floor(s2.player.x / (double)CHUNK_SIZE);
			w_coord_t player_cz
				= (w_coord_t)floor(s2.player.z / (double)CHUNK_SIZE);
			assert(server_world_load_chunk(
				&s2.worlds[0], player_cx, player_cz, &player_chunk) && player_chunk);
		}
		w_coord_t quit_edge_cx = 0, quit_edge_cz = 0;
		int quit_edge_dx = 0, quit_edge_dz = 0;
		bool quit_edge_found = false;
		dict_server_chunks_it_t quit_chunk_it;
		dict_server_chunks_it(quit_chunk_it, s2.worlds[0].chunks);
		while(!dict_server_chunks_end_p(quit_chunk_it) && !quit_edge_found) {
			int64_t cid = dict_server_chunks_ref(quit_chunk_it)->key;
			w_coord_t cx = S_CHUNK_X(cid), cz = S_CHUNK_Z(cid);
			static const int dirs[4][2] = {
				{1, 0}, {-1, 0}, {0, 1}, {0, -1},
			};
			for(size_t d = 0; d < 4; d++) {
				if(!server_world_is_chunk_loaded(
					   &s2.worlds[0], cx + dirs[d][0], cz + dirs[d][1])) {
					quit_edge_cx = cx;
					quit_edge_cz = cz;
					quit_edge_dx = dirs[d][0];
					quit_edge_dz = dirs[d][1];
					quit_edge_found = true;
					break;
				}
			}
			dict_server_chunks_next(quit_chunk_it);
		}
		assert(quit_edge_found);
		vec3 quit_edge_pos = {
			quit_edge_cx * CHUNK_SIZE
				+ (quit_edge_dx < 0 ? 0.25f : quit_edge_dx > 0 ? 15.75f : 8.5f),
			70.0f,
			quit_edge_cz * CHUNK_SIZE
				+ (quit_edge_dz < 0 ? 0.25f : quit_edge_dz > 0 ? 15.75f : 8.5f),
		};
		uint32_t quit_edge_id = server_local_gen_id(&s2);
		struct entity* quit_edge = malloc(sizeof(*quit_edge));
		assert(quit_edge);
		entity_item(quit_edge_id, quit_edge, true, &s2.worlds[0],
			(struct item_data) {.id = ITEM_STICK, .durability = 12345, .count = 7});
		quit_edge->teleport(quit_edge, quit_edge_pos);
		quit_edge->vel[0] = quit_edge_dx * 0.75f;
		quit_edge->vel[1] = 0.0f;
		quit_edge->vel[2] = quit_edge_dz * 0.75f;
		quit_edge->data.item.age = 77;
		*dict_entity_safe_get(s2.entities, quit_edge_id) = quit_edge;
		server_local_refresh_entity_membership(&s2, quit_edge);
		assert(quit_edge->added_to_chunk
			   && quit_edge->chunk_coord_x == quit_edge_cx
			   && quit_edge->chunk_coord_z == quit_edge_cz);
		assert(server_local_test_tick_entities(&s2));
		assert(quit_edge->pos[0] == quit_edge_pos[0]
			   && quit_edge->pos[2] == quit_edge_pos[2]);
		assert(quit_edge->data.item.age == 77 && quit_edge->added_to_chunk);
		/* Save and quit must accept CaveWii's documented transient fishing hook
		 * through the complete production checkpoint/release path. The hook stays
		 * live until all fallible durability stages commit, then world teardown
		 * releases it and clears the player's handle. */
		uint32_t quit_fish_id = server_local_gen_id(&s2);
		struct entity* quit_fish = malloc(sizeof(*quit_fish));
		assert(quit_fish);
		entity_fish(quit_fish_id, quit_fish, true, &s2.worlds[0]);
		quit_fish->teleport(quit_fish,
			(vec3) {(float)s2.player.x, (float)s2.player.y,
					(float)s2.player.z});
		*dict_entity_safe_get(s2.entities, quit_fish_id) = quit_fish;
		s2.player.fish_entity_id = quit_fish_id;
		server_local_unload_world_impl(&s2, true);
		assert(!s2.player.has_pos && s2.player.fish_entity_id == 0);
		assert(dict_entity_size(s2.entities) == 0);
		assert(!s2.player.sleeping && s2.player.sleep_timer == 0);
		assert(s2.player.sleep_x == 0 && s2.player.sleep_y == 0
			   && s2.player.sleep_z == 0);
		/* The long-lived server struct must not lend prior-world knockback or
		 * damage immunity to a fresh ordinary/menu load. Live portal loads use the
		 * separate preserve/restore path tested above. */
		s2.player.vel_x = 0.75f;
		s2.player.vel_z = -0.5f;
		s2.player.hurt_resist_time = 13;
		s2.player.last_damage = 47;
		assert(server_local_load_world_impl(&s2, lvl2));
		struct server_chunk* quit_edge_chunk = NULL;
		assert(server_world_load_chunk(&s2.worlds[0], quit_edge_cx, quit_edge_cz,
			&quit_edge_chunk) && quit_edge_chunk);
		size_t quit_edge_restored = 0;
		dict_entity_it_t quit_entity_it;
		dict_entity_it(quit_entity_it, s2.entities);
		while(!dict_entity_end_p(quit_entity_it)) {
			struct entity* e = dict_entity_ref(quit_entity_it)->value;
			if(e && e->type == ENTITY_ITEM
			   && e->data.item.item.id == ITEM_STICK
			   && e->data.item.item.durability == 12345
			   && e->data.item.item.count == 7) {
				quit_edge_restored++;
				assert(e->data.item.age == 77);
				assert(e->pos[0] == quit_edge_pos[0]
					   && e->pos[2] == quit_edge_pos[2]);
			}
			dict_entity_next(quit_entity_it);
		}
		assert(quit_edge_restored == 1);
		assert(s2.player.vel_x == 0.0f && s2.player.vel_y == 0.0f
			   && s2.player.vel_z == 0.0f);
		assert(s2.player.hurt_resist_time == 0 && s2.player.last_damage == 0);
		server_local_test_finish_load_inventory(&s2);
		assert(inventory_get_slot(&s2.player.inventory, INVENTORY_SLOT_HOTBAR,
								  &load_slot));
		/* debug.give kit sentinel: hotbar slot 0 is FLINT & STEEL since the
		   2026-07-28 full-inventory kit (was BLOCK_CHEST x64). */
		assert(load_slot.id == ITEM_FLINT_STEEL && load_slot.count == 1);
		server_local_unload_world_impl(&s2, true);

		/* Imported/corrupt worlds can have a missing, partial, or wrong-type
		 * Data.SpawnXYZ triple. Production owns server_local as an uninitialized
		 * automatic object, so ignored read failures formerly left arbitrary
		 * permanent respawn coordinates. Corrupt one member, poison the prior
		 * runtime values, and prove load/death use the all-or-nothing fallback. */
		{
			struct level_archive malformed_spawn;
			assert(level_archive_create(&malformed_spawn, lvl2));
			nbt_node* spawn_y = nbt_find_by_path(
				malformed_spawn.data, LEVEL_WORLD_SPAWNY.name);
			assert(spawn_y && spawn_y->type == TAG_INT);
			spawn_y->type = TAG_SHORT;
			malformed_spawn.modified = true;
			level_archive_destroy(&malformed_spawn);
		}
		s2.player.spawn_x = 1234567;
		s2.player.spawn_y = -12345;
		s2.player.spawn_z = -7654321;
		assert(server_local_load_world_impl(&s2, lvl2));
		assert(s2.player.spawn_x == 0 && s2.player.spawn_y == 80
			   && s2.player.spawn_z == 0);
		server_local_test_finish_load_inventory(&s2);
		s2.player.has_bed_spawn = false;
		s2.world_switch_phase = SERVER_WORLD_SWITCH_IDLE;
		s2.player.finished_loading = true;
		assert(server_local_set_player_health(&s2, 0));
		/* Deviation-#18 split: the placement belongs to the respawn. */
		assert(server_local_respawn_request(&s2));
		assert(s2.player.x == 0.5
			   && s2.player.y == 80 + PLAYER_EYE_HEIGHT
			   && s2.player.z == 0.5);
		server_local_unload_world_impl(&s2, true);
		puts("  spawn: malformed world triple loads/deaths at deterministic 0,80,0 fallback");
		string_clear(lvl2);
		/* The last checked unload can legitimately fail closed when this fault-
		 * injection fixture leaves an entity outside the resident set. At process
		 * teardown there is no persistence obligation left, so explicitly discard
		 * that still-live synthetic world before releasing the session owner. */
		if(s2.player.has_pos) {
			server_world_discard(&s2.worlds[0]);
			level_archive_discard(&s2.level);
			s2.player.has_pos = false;
		}
		dict_entity_it_t teardown_it;
		dict_entity_it(teardown_it, s2.entities);
		while(!dict_entity_end_p(teardown_it)) {
			free(dict_entity_ref(teardown_it)->value);
			dict_entity_next(teardown_it);
		}
		dict_entity_clear(s2.entities);
		inventory_destroy(&s2.player.inventory, &s2.player);
		piston_moving_store_destroy(&s2.piston_moving);
		server_world_populate_scratch_destroy(session_populate_scratch);
		s2.world_populate_scratch = NULL;
		printf("  portal: ROUND TRIP (player + dropped-entity persistence) OK\n");
	}

	// CRPC_WORLD_RESET must invalidate old-world aliases even when the first
	// new-world allocation fails. This models Wii heap exhaustion under NDEBUG.
	{
		/* The preceding threadless server integration deliberately queues real
		 * client RPCs while a helper normally discards them. Establish an exact
		 * empty FIFO before this client-only fixture: its manual clin_update calls
		 * must not consume a late integration marker into the replacement gstate. */
		clin_drain_discard();
		memset(&gstate, 0, sizeof(gstate));
		world_create(&gstate.world);
		dict_entity_init(gstate.entities);
		particle_init();
		struct entity** stale = dict_entity_safe_get(gstate.entities, 77);
		*stale = calloc(1, sizeof(**stale));
		assert(*stale);
		gstate.players[0].local_player = *stale;
		gstate.world_loaded = true;

		struct client_rpc reset = {
			.type = CRPC_WORLD_RESET,
			.payload.world_reset.dimension = WORLD_DIM_NETHER,
			.payload.world_reset.local_entity = 0,
		};
		// A successful reset creates a fresh, slot-0-selected client inventory.
		// The server's post-load sync must be able to restore the carried selected
		// hotbar slot as well as item stacks.
		clin_process(&reset);
		assert(gstate.players[0].windows[WINDOWC_INVENTORY]);
		assert(!gstate.world_loaded);
		struct item_data synced[INVENTORY_SIZE];
		for(size_t i = 0; i < INVENTORY_SIZE; i++) {
			synced[i] = (struct item_data) {
				.id = BLOCK_STONE,
				.durability = (int16_t)i,
				.count = (uint8_t)(i + 1),
			};
			struct client_rpc slot = {
				.type = CRPC_INVENTORY_SLOT,
				.payload.inventory_slot = {
					.window = WINDOWC_INVENTORY,
					.slot = (uint8_t)i,
					.item = synced[i],
				},
			};
			clin_process(&slot);
			assert(!gstate.world_loaded);
		}
		struct client_rpc hotbar = {
			.type = CRPC_PLAYER_HOTBAR_SLOT,
			.payload.player_hotbar_slot.slot = 7,
		};
		clin_process(&hotbar);
		assert(inventory_get_hotbar(windowc_get_latest(
				   gstate.players[0].windows[WINDOWC_INVENTORY]))
			   == 7);

		/* The checked M-lib reserve transaction is exercised directly above. Use
		 * the capacity-independent admission seam here to prove both a gameplay
		 * spawn and WORLD_RESET fail closed before safe_get. */
		dict_entity_it_t empty_it;
		dict_entity_it(empty_it, gstate.entities);
		while(!dict_entity_end_p(empty_it)) {
			free(dict_entity_ref(empty_it)->value);
			dict_entity_next(empty_it);
		}
		dict_entity_clear(gstate.entities);
		dict_entity_init(gstate.entities);
		gstate.players[0].local_player = NULL;
		struct client_rpc oom_spawn = {
			.type = CRPC_SPAWN_ITEM,
			.payload.spawn_item = {
				.entity_id = 400,
				.item = {.id = ITEM_APPLE, .count = 1},
				.pos = {0.5f, 70.0f, 0.5f},
			},
		};
		/* Fill just below M-lib's insertion threshold, then erase every live key.
		 * live_size+1 no longer requests growth, but count_delete+1 does. The
		 * checked preflight must see that tombstone threshold and absorb the
		 * injected realloc failure before safe_get. */
		size_t tombstone_count = (size_t)gstate.entities->upper_limit - 1;
		assert(tombstone_count > 0);
		for(size_t i = 0; i < tombstone_count; i++)
			*dict_entity_safe_get(gstate.entities, 5000 + (uint32_t)i) = NULL;
		for(size_t i = 0; i < tombstone_count; i++)
			assert(dict_entity_erase(gstate.entities, 5000 + (uint32_t)i));
		assert(dict_entity_size(gstate.entities) == 0);
		assert((size_t)gstate.entities->count_delete + 1
			   >= (size_t)gstate.entities->upper_limit);
		clin_test_fail_entity_dict_realloc_after(1);
		clin_process(&oom_spawn);
		assert(dict_entity_size(gstate.entities) == 0);

		clin_test_fail_entity_slot_after(1);
		clin_process(&oom_spawn);
		assert(dict_entity_size(gstate.entities) == 0);

		clin_test_fail_entity_slot_after(1);
		clin_process(&reset);
		assert(!gstate.world_loaded && gstate.world_switch_pending);
		assert(!gstate.players[0].local_player);
		assert(dict_entity_size(gstate.entities) == 0);
		assert(!gstate.players[0].windows[WINDOWC_INVENTORY]);
		clin_process(&reset); // failure countdown consumed; restore baseline
		assert(gstate.players[0].local_player && gstate.players[0].windows[WINDOWC_INVENTORY]);
		for(size_t i = 0; i < INVENTORY_SIZE; i++) {
			struct client_rpc slot = {
				.type = CRPC_INVENTORY_SLOT,
				.payload.inventory_slot = {
					.window = WINDOWC_INVENTORY,
					.slot = (uint8_t)i,
					.item = synced[i],
				},
			};
			clin_process(&slot);
		}
		clin_process(&hotbar);
		struct client_rpc pos = {
			.type = CRPC_PLAYER_POS,
			.payload.player_pos.position = {12.5f, 70.0f, -7.5f},
		};
		assert(!gstate.world_loaded);
		clin_process(&pos);
		assert(gstate.world_loaded);
		for(size_t i = 0; i < INVENTORY_SIZE; i++) {
			struct item_data observed;
			assert(inventory_get_slot(windowc_get_latest(
					   gstate.players[0].windows[WINDOWC_INVENTORY]), i, &observed));
			assert(observed.id == synced[i].id
				   && observed.durability == synced[i].durability
				   && observed.count == synced[i].count);
		}

		/* Exact video path: CRPC_SPAWN_ITEM allocates an EntityItem from raw
		 * client heap storage, raycast selects it, then screen_ingame calls a
		 * non-NULL click callback. Poisoning the production allocator makes the
		 * former commented-out callback initializers deterministic (0xBEBEBEBE). */
		clin_set_alloc_hook(poison_client_alloc);
		clin_process(&oom_spawn);
		clin_set_alloc_hook(NULL);
		struct entity** spawned_item
			= dict_entity_get(gstate.entities, oom_spawn.payload.spawn_item.entity_id);
		assert(spawned_item && *spawned_item
			   && (*spawned_item)->type == ENTITY_ITEM);
		assert((*spawned_item)->onLeftClick == NULL
			   && (*spawned_item)->onRightClick == NULL
			   && "client item spawn must clear raw-heap click callbacks");

		// BEGIN_WORLD_SWITCH must hold the loading screen even though world_loaded
		// remains true and retain every old-world alias for a checkpoint rollback.
		// A reversible cancel closes through an authoritative same-world epoch,
		// not an untagged RETURN, so release edges consumed while loading cannot
		// become a source-world dig/place after cancellation.
		gstate.players[0].current_screen = &screen_ingame;
		gstate.paused = true; // optimistic HOME press racing server-side BEGIN
		struct entity* live_player = gstate.players[0].local_player;
		struct window_container* live_inventory
			= gstate.players[0].windows[WINDOWC_INVENTORY];
		size_t live_entities = dict_entity_size(gstate.entities);
		struct client_rpc begin_switch = {.type = CRPC_BEGIN_WORLD_SWITCH};
		clin_process(&begin_switch);
		assert(loading_reason_get(0) == (gstate.world.dimension == WORLD_DIM_NETHER
			? PORTAL_OUT : PORTAL_IN));
		assert(gstate.world_switch_pending);
		assert(!gstate.paused);
		assert(gstate.players[0].current_screen == &screen_load_world);
		assert(gstate.world_loaded && gstate.players[0].local_player == live_player);
		assert(gstate.players[0].windows[WINDOWC_INVENTORY] == live_inventory);
		assert(dict_entity_size(gstate.entities) == live_entities);
		screen_load_world.update(&screen_load_world, 0.05f);
		assert(gstate.players[0].current_screen == &screen_load_world);
		gstate.players[0].digging.active = true;
		gstate.players[0].held_item_animation.punch.place = true;
		gstate.players[0].camera_hit.hit = true;
		gstate.players[0].camera_hit.entity_hit = true;
		gstate.players[0].camera_hit.entity_id = 123;
		gstate.autopilot.pending_place = 2;
		world_set_block(
			&gstate.world, 12, 69, -8,
			(struct block_data) {.type = BLOCK_STONE}, false);
		struct client_rpc cancel_switch = {
			.type = CRPC_PLAYER_POS,
			.payload.player_pos = {
				.position = {12.5f, 70.0f, -7.5f},
				.rotation = {0.0f, 0.0f},
				.velocity = {0.25f, -0.5f, 0.75f},
				.world_time = 12345,
				.input_epoch = 222,
				.oxygen = 44,
				.selected_hotbar = 7,
				.on_fire = true,
				.fresh_body = false,
			},
		};
		clin_process(&cancel_switch);
		assert(gstate.world_switch_pending);
		assert(!gstate.paused);
		assert(gstate.players[0].current_screen == &screen_load_world);
		screen_load_world.update(&screen_load_world, 30.0f);
		clin_update_input_readiness();
		assert(gstate.players[0].current_screen == &screen_ingame);
		assert(gstate.world_loaded && gstate.players[0].local_player == live_player);
		assert(gstate.players[0].windows[WINDOWC_INVENTORY] == live_inventory);
		assert(dict_entity_size(gstate.entities) == live_entities);
		assert(!gstate.players[0].digging.active);
		assert(!gstate.players[0].held_item_animation.punch.place);
		assert(!gstate.players[0].camera_hit.hit && !gstate.players[0].camera_hit.entity_hit
			   && gstate.players[0].camera_hit.entity_id == 0);
		assert(gstate.autopilot.pending_place == 0);
		assert(gstate.players[0].local_player->vel[0] == 0.25f
			   && gstate.players[0].local_player->vel[1] == -0.5f
			   && gstate.players[0].local_player->vel[2] == 0.75f);
		assert(gstate.players[0].local_player->on_fire && gstate.players[0].oxygen == 44);
		assert(gstate.world_time == 12345);
		struct input_gate_capture cancel_ack_capture = {
			.ack_index = SIZE_MAX,
		};
		svin_process_messages(
			capture_input_gate_transport, &cancel_ack_capture, false);
		assert(cancel_ack_capture.count == 1);
		assert(cancel_ack_capture.ack_index == 0
			   && cancel_ack_capture.ack_epoch == 222);
		assert(loading_reason_get(0) == UNKNOWN);
		puts("  portal: reversible loading feedback preserves live client aliases");

		// A non-world-reset GUI open has the same two allocation layers. On
		// either failure the client stays ingame and reliably rejects the hidden
		// authoritative server container with SRPC_WINDOW_CLOSE.
		struct client_rpc open_workbench = {
			.type = CRPC_OPEN_WINDOW,
			.payload.window_open = {
				.window = WINDOWC_CRAFTING,
				.type = WINDOW_TYPE_WORKBENCH,
				.slot_count = CRAFTING_SIZE,
			},
		};
		struct client_rpc rebuild_workbench = {
			.type = CRPC_REOPEN_WINDOW,
			.payload.window_reopen = {
				.window = WINDOWC_CRAFTING,
				.type = WINDOW_TYPE_WORKBENCH,
				.slot_count = CRAFTING_SIZE,
			},
		};

		/* HOME owns the screen before its TOGGLE reaches the server. An OPEN from
		 * an earlier block click must close the hidden server container without
		 * replacing pause; a failed CLOSE/REOPEN rebuilds locally but waits for a
		 * later unpaused ingame frame before becoming visible. */
		assert(!gstate.players[0].windows[WINDOWC_CRAFTING]);
		gstate.paused = true;
		gstate.players[0].current_screen = &screen_pause;
		clin_process(&open_workbench);
		assert(gstate.players[0].current_screen == &screen_pause);
		assert(!gstate.players[0].windows[WINDOWC_CRAFTING]);
		struct close_transport_capture paused_open_close = {0};
		svin_process_messages(
			capture_close_transport, &paused_open_close, false);
		while(svin_pending_count() > 0) {
			svin_rpc_flush_pending();
			svin_process_messages(
				capture_close_transport, &paused_open_close, false);
		}
		assert(paused_open_close.close_count == 1);
		assert(paused_open_close.action_count == 0);

		clin_process(&rebuild_workbench);
		assert(gstate.players[0].windows[WINDOWC_CRAFTING]);
		assert(gstate.players[0].current_screen == &screen_pause && gstate.paused);
		assert(!gstate.world_switch_pending && gstate.world_loaded);
		gstate.paused = false;
		gstate.players[0].current_screen = &screen_ingame;
		clin_drain_discard();
		clin_update();
		assert(!gstate.world_switch_pending);
		assert(gstate.players[0].current_screen == &screen_crafting);

		/* Save/select and world-switch loading invalidate deferred ownership.
		 * Presentation-only loading instead retains it (tested above). */
		gstate.paused = true;
		gstate.players[0].current_screen = &screen_pause;
		clin_process(&rebuild_workbench);
		gstate.paused = false;
		gstate.players[0].current_screen = &screen_select_world;
		clin_drain_discard();
		clin_update();
		assert(gstate.players[0].current_screen == &screen_select_world);
		gstate.players[0].current_screen = &screen_ingame;
		clin_drain_discard();
		clin_update();
		assert(gstate.players[0].current_screen == &screen_ingame);

		gstate.paused = true;
		gstate.players[0].current_screen = &screen_pause;
		clin_process(&rebuild_workbench);
		gstate.paused = false;
		gstate.players[0].current_screen = &screen_load_world;
		gstate.world_switch_pending = true;
		clin_drain_discard();
		clin_update();
		assert(gstate.players[0].current_screen == &screen_load_world);
		gstate.world_switch_pending = false;
		gstate.players[0].current_screen = &screen_ingame;
		clin_drain_discard();
		clin_update();
		assert(gstate.players[0].current_screen == &screen_ingame);
		/* clin_update may emit its periodic position sample while exercising the
		 * deferred reveal. Keep the following allocation-failure transport checks
		 * independent of wall-clock test speed. */
		struct close_transport_capture paused_resume_transport = {0};
		svin_process_messages(
			capture_close_transport, &paused_resume_transport, false);
		while(svin_pending_count() > 0) {
			svin_rpc_flush_pending();
			svin_process_messages(
				capture_close_transport, &paused_resume_transport, false);
		}
		assert(paused_resume_transport.close_count == 0);

		clin_set_alloc_hook(fail_client_alloc);
		clin_process(&open_workbench);
		clin_set_alloc_hook(NULL);
		assert(!gstate.players[0].windows[WINDOWC_CRAFTING]);
		windowc_set_alloc_hook(fail_client_alloc);
		clin_process(&open_workbench);
		windowc_set_alloc_hook(NULL);
		assert(!gstate.players[0].windows[WINDOWC_CRAFTING]);
		clin_process(&rebuild_workbench);
		assert(gstate.players[0].windows[WINDOWC_CRAFTING]);
		assert(gstate.players[0].current_screen == &screen_crafting);

        /* A failed replacement must not resurrect a different deferred menu. */
        struct close_transport_capture open_failures={0};
        svin_process_messages(capture_close_transport,&open_failures,false);
        while(svin_pending_count()>0){svin_rpc_flush_pending();svin_process_messages(capture_close_transport,&open_failures,false);}
        assert(open_failures.close_count==2);
        for(unsigned allocation=0;allocation<2;allocation++) {
            gstate.players[0].current_screen=&screen_ingame;
            gstate.players[0].visible_window=WINDOWC_INVENTORY;
            gstate.players[0].deferred_reopen_window=WINDOWC_INVENTORY;
            gstate.players[0].menu_host.mode=LOCAL_MENU_CONTAINER;
            struct client_rpc replacement={.type=CRPC_REOPEN_WINDOW,
                .payload.window_reopen={.window=WINDOWC_CRAFTING,.type=WINDOW_TYPE_CHEST,.slot_count=63}};
            if(allocation==0)clin_set_alloc_hook(fail_client_alloc);
            else windowc_set_alloc_hook(fail_client_alloc);
            clin_process(&replacement);
            clin_set_alloc_hook(NULL);windowc_set_alloc_hook(NULL);
            assert(!gstate.players[0].windows[WINDOWC_CRAFTING]);
            assert(!local_menu_host_active(&gstate.players[0]));
            assert(gstate.players[0].visible_window==-1&&gstate.players[0].deferred_reopen_window==-1);
            clin_update();assert(gstate.players[0].current_screen==&screen_ingame);
            struct close_transport_capture failed_reopen={0};
            svin_process_messages(capture_close_transport,&failed_reopen,false);
            while(svin_pending_count()>0){svin_rpc_flush_pending();svin_process_messages(capture_close_transport,&failed_reopen,false);}
            assert(failed_reopen.close_count==1);
            clin_process(&rebuild_workbench);
            assert(gstate.players[0].windows[WINDOWC_CRAFTING]);
        }
		struct client_rpc reopen = {
			.type = CRPC_REOPEN_WINDOW,
			.payload.window_reopen = {
				.window = WINDOWC_INVENTORY,
				.type = WINDOW_TYPE_INVENTORY,
				.slot_count = INVENTORY_SIZE,
			},
		};
		// An OPEN from a queued block click, or a REOPEN from a failed CLOSE, can
		// arrive just before an already-queued UNLOAD succeeds. Neither stale
		// message may pull world-select into a render-world screen, or the
		// following unload-only reset switches to a load screen that will never
		// receive PLAYER_POS.
		gstate.players[0].current_screen = &screen_select_world;
		struct window_container* stale_open_window
			= gstate.players[0].windows[WINDOWC_CRAFTING];
		clin_process(&open_workbench);
		assert(gstate.players[0].current_screen == &screen_select_world);
		assert(gstate.players[0].windows[WINDOWC_CRAFTING] == stale_open_window);
		clin_process(&reopen);
		assert(gstate.players[0].current_screen == &screen_select_world);
		/* A REOPEN already deferred behind pause must also die at WORLD_RESET;
		 * the replacement epoch may not resurrect the old container screen. */
		gstate.paused = true;
		gstate.players[0].current_screen = &screen_pause;
		clin_process(&reopen);
		assert(gstate.players[0].current_screen == &screen_pause);
		gstate.paused = false;
		gstate.players[0].current_screen = &screen_select_world;
		clin_process(&reset);
		assert(gstate.players[0].current_screen == &screen_select_world);
		assert(!gstate.world_loaded);
		/* WORLD_RESET is sticky until an epoch marker, even if a legacy untagged
		 * position arrives. Restore this fixture through the real completion edge. */
		world_set_block(
			&gstate.world, 12, 69, -8,
			(struct block_data) {.type = BLOCK_STONE}, false);
		struct client_rpc completed_pos = pos;
		completed_pos.payload.player_pos.input_epoch = 223;
		completed_pos.payload.player_pos.oxygen = MAX_OXYGEN;
		completed_pos.payload.player_pos.selected_hotbar = 7;
		clin_process(&completed_pos);
		assert(gstate.world_loaded);
		assert(gstate.world_switch_pending);
		assert(gstate.players[0].current_screen == &screen_load_world);
		screen_load_world.update(&screen_load_world, 30.0f);
		clin_update_input_readiness();
		assert(gstate.players[0].current_screen == &screen_ingame);
		gstate.players[0].current_screen = &screen_load_world;
		clin_process(&reopen);
		assert(gstate.players[0].current_screen == &screen_load_world);
		gstate.players[0].current_screen = &screen_select_world;
		struct client_rpc unload_cancelled = {.type = CRPC_RETURN_INGAME};
		clin_process(&unload_cancelled);
		assert(gstate.players[0].current_screen == &screen_ingame);
		clin_process(&reopen);
		assert(gstate.players[0].current_screen == &screen_inventory);

		// A failed authoritative close of window 0 may arrive after the client
		// already discarded that window. REOPEN must be able to reconstruct the
		// player container, then accept its out-of-band cursor + hotbar snapshot.
		windowc_destroy(gstate.players[0].windows[WINDOWC_INVENTORY]);
		free(gstate.players[0].windows[WINDOWC_INVENTORY]);
		gstate.players[0].windows[WINDOWC_INVENTORY] = NULL;
		gstate.players[0].current_screen = &screen_ingame;
		clin_process(&reopen);
		assert(gstate.players[0].windows[WINDOWC_INVENTORY]);
		assert(gstate.players[0].current_screen == &screen_inventory);
		struct client_rpc reopened_slot = {
			.type = CRPC_INVENTORY_SLOT,
			.payload.inventory_slot = {
				.window = WINDOWC_INVENTORY,
				.slot = INVENTORY_SLOT_CRAFTING,
				.item = {.id = ITEM_BREAD, .durability = 9, .count = 2},
			},
		};
		struct client_rpc reopened_cursor = {
			.type = CRPC_INVENTORY_SLOT,
			.payload.inventory_slot = {
				.window = WINDOWC_INVENTORY,
				.slot = SPECIAL_SLOT_PICKED_ITEM,
				.item = {.id = ITEM_COAL, .durability = 4, .count = 3},
			},
		};
		struct client_rpc reopened_hotbar = {
			.type = CRPC_PLAYER_HOTBAR_SLOT,
			.payload.player_hotbar_slot.slot = 7,
		};
		clin_process(&reopened_slot);
		clin_process(&reopened_cursor);
		clin_process(&reopened_hotbar);
		struct item_data reopened_item;
		assert(inventory_get_slot(
			windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]),
			INVENTORY_SLOT_CRAFTING, &reopened_item));
		assert(reopened_item.id == ITEM_BREAD && reopened_item.durability == 9
			   && reopened_item.count == 2);
		assert(inventory_get_picked_item(
			windowc_get_latest(gstate.players[0].windows[WINDOWC_INVENTORY]),
			&reopened_item));
		assert(reopened_item.id == ITEM_COAL && reopened_item.durability == 4
			   && reopened_item.count == 3);
		assert(inventory_get_hotbar(windowc_get_latest(
				   gstate.players[0].windows[WINDOWC_INVENTORY]))
			   == 7);
		/* Earlier GUI allocation cases deliberately queued lifecycle replies. Drain
		 * them so the missing-owner NACK assertion below is independent of capacity. */
		struct input_gate_capture before_owner_oom = {.ack_index = SIZE_MAX};
		svin_process_messages(
			capture_input_gate_transport, &before_owner_oom, false);
		while(svin_pending_count() > 0) {
			svin_rpc_flush_pending();
			svin_process_messages(
				capture_input_gate_transport, &before_owner_oom, false);
		}
		svin_process_messages(
			capture_input_gate_transport, &before_owner_oom, false);

		// WORLD_RESET is a committed teardown but remains input/screen-pending until
		// the final epoch marker. An allocation failure must retain that safe gate.
		clin_process(&begin_switch);
		assert(gstate.world_switch_pending);
		gstate.paused = true;
		clin_set_alloc_hook(fail_client_alloc);
		clin_process(&reset);
		clin_set_alloc_hook(NULL);

		assert(!gstate.world_loaded);
		assert(gstate.world_switch_pending);
		assert(!gstate.paused);
		assert(!gstate.players[0].local_player);
		assert(dict_entity_size(gstate.entities) == 0);
		assert(!gstate.players[0].windows[WINDOWC_INVENTORY]);
		// The server still queues its normal post-stream completion RPCs. A
		// failed reset is sticky: neither the position nor inventory sync may
		// enter gameplay or resurrect a partial client world.
		clin_process(&pos);
		clin_process(&hotbar);
		assert(!gstate.world_loaded && !gstate.players[0].local_player);
		assert(!gstate.players[0].windows[WINDOWC_INVENTORY]);
		/* The real final marker is tagged. Missing reset owners must NACK it rather
		 * than silently strand the server's epoch barrier; repeated server retries
		 * then converge on the visible terminal-load RPC. */
		struct client_rpc missing_owner_marker = pos;
		missing_owner_marker.payload.player_pos.input_epoch = 224;
		missing_owner_marker.payload.player_pos.oxygen = MAX_OXYGEN;
		missing_owner_marker.payload.player_pos.selected_hotbar = 0;
		clin_process(&missing_owner_marker);
		assert(gstate.players[0].current_screen == &screen_load_world);
		assert(gstate.world_switch_pending && !gstate.world_loaded);
		struct input_gate_capture missing_owner_retry = {.ack_index = SIZE_MAX};
		svin_process_messages(
			capture_input_gate_transport, &missing_owner_retry, false);
		while(svin_pending_count() > 0) {
			svin_rpc_flush_pending();
			svin_process_messages(
				capture_input_gate_transport, &missing_owner_retry, false);
		}
		svin_process_messages(
			capture_input_gate_transport, &missing_owner_retry, false);
		assert(missing_owner_retry.ack_epoch == 224);
		assert(missing_owner_retry.retry);

		// Fail the second reset allocation (the local player) as a separate
		// path. The newly created inventory window must be rolled back too.
		clin_process(&reset); // restore a complete baseline first
		assert(gstate.players[0].local_player && gstate.players[0].windows[WINDOWC_INVENTORY]);
		client_allocs_until_failure = 2;
		clin_set_alloc_hook(fail_nth_client_alloc);
		clin_process(&reset);
		clin_set_alloc_hook(NULL);
		clin_process(&pos);
		assert(!gstate.world_loaded && !gstate.players[0].local_player);
		assert(dict_entity_size(gstate.entities) == 0);
		assert(!gstate.players[0].windows[WINDOWC_INVENTORY]);

		// The outer window allocation can succeed while its first revision
		// snapshot fails. windowc_create reports that failure; reset readiness
		// must include it rather than exposing a nonnull-but-empty container.
		clin_process(&reset); // complete baseline after the player-allocation case
		assert(gstate.players[0].local_player && gstate.players[0].windows[WINDOWC_INVENTORY]);
		windowc_set_alloc_hook(fail_client_alloc);
		clin_process(&reset);
		windowc_set_alloc_hook(NULL);
		clin_process(&pos);
		assert(!gstate.world_loaded && !gstate.players[0].local_player);
		assert(dict_entity_size(gstate.entities) == 0);
		assert(!gstate.players[0].windows[WINDOWC_INVENTORY]);
		world_destroy(&gstate.world);
		dict_entity_clear(gstate.entities);
		printf("  portal: client WORLD_RESET OOM leaves no stale aliases OK\n");
	}

	server_world_destroy(&s.world);
	dict_entity_clear(s.entities);
	string_clear(level);
	g_drain_run = 0;
	thread_join(&drain);
	test_fixture_dir_cleanup(TMP_WORLD);
	test_fixture_dir_cleanup(TEST_PORTAL_TMP2);
	printf("test_portal_travel (ignition + self-break + dwell + teleporter + round trip): ALL OK\n");
	return 0;
}
