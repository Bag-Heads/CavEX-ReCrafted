/*
	Golden test for Beta 1.7.3 sign placement (ItemSign.onItemUse), item 323.

	Pins:
	  - side-face use (SIDE_LEFT/RIGHT/FRONT/BACK) against a solid clicked block
	    places BLOCK_SIGN (68, Beta's signWall) at the target cell with Beta's
	    raw metadata (4/5/2/3 respectively -- ItemSign's clicked-side value).
	  - top-face use places BLOCK_SIGN_POST (63) with Beta's 0..15 yaw metadata,
	    including metadata 4 where the old metadata-bit discriminator would have
	    mistaken the post for a wall sign.
	  - bottom-face use is REFUSED [740c583 ItemSign.onItemUse `if (var7==0)
	    return false;`].
	  - a non-solid clicked block (tall grass) refuses placement [740c583
	    `!getBlockMaterial(...).isSolid()`].
	  - a non-replaceable target cell (already occupied by a solid, non-
	    place_ignore block) refuses placement (Beta's inherited
	    Block.canPlaceBlockAt).
	  - the yaw -> signPost 16-direction formula (MathHelper.floor_double(
	    (yaw+180)*16/360 + 0.5) & 15) is pinned at several angles including a
	    negative-yaw case that exercises floor_double's floor-toward-negative-
	    infinity (vs a naive truncating (int) cast).
	  - block-id (not metadata) selects post bounds/support/rendering; block 68's
	    old 4..7 encoding and Beta's 2..5 encoding are compared against one
	    frozen physical-facing table for bounds, support, mesh, and text.
	  - provenance-safe save migration fixtures cover tagless real-Beta data,
	    save-stamped-v1 real-Beta data, CaveWii-created legacy 4..7 data, and an
	    ambiguous tagless 4/5 world. Every case is checked by physical wall, not
	    merely by raw metadata; the legacy case also pins crash-safe retry.
	  - the full SRPC_BLOCK_PLACE dispatch (server_local_test_process_rpc)
	    consumes the held sign stack ONLY on a successful placement, and opens
	    the sign edit GUI (WINDOWC_SIGN/inventory_logic_sign) immediately after
	    -- Beta's `--stackSize` + `displayGUIEditSign` tail, reusing the
	    EXISTING sign-OSK open call (block_sign.c's onRightClick) with no new
	    RPC plumbing; a refused placement leaves both untouched.

	Decompile: 740c583:1.7.3-LTS ItemSign.onItemUse, BlockSign.java,
	MathHelper.floor_double, Block.canPlaceBlockAt.
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
#include "block/blocks_data.h"
#include "block/piston_mobility.h"
#include "block/redstone_piston.h"
#include "chunk.h"
#include "entity/entity.h"
#include "item/inventory.h"
#include "item/items.h"
#include "item/items_object.h"
#include "item/tool.h"
#include "network/client_interface.h"
#include "network/inventory_logic.h"
#include "network/level_archive.h"
#include "network/region_archive.h"
#include "network/server_interface.h"
#include "network/server_local.h"
#include "network/server_world.h"
#include "graphics/render_sign_text.h"
#include "platform/mem2.h"
#include "platform/thread.h"
#include "platform/time.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_sign_placement_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
static char s_fixture_MIGRATION_WORLD[128]; /* was #define MIGRATION_WORLD='test_sign_meta_migration_tmp' (a literal) */
#define MIGRATION_WORLD s_fixture_MIGRATION_WORLD
#define PC_VERTEX_BYTES 22
#define COLUMN_BLOCKS (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)

struct wall_facing_equiv {
	enum side placement_side;
	uint8_t legacy_meta;
	uint8_t beta_meta;
	int support_dx;
	int support_dz;
	float box[6];
	enum side render_front;
	int render_axis;
	float render_plane;
	int text_rotation_deg;
};

// Physical facing/render/text migration is frozen from the pre-renumber table.
// Selection boxes are Beta BlockSign.setBlockBoundsBasedOnState (java:28-50):
// y 9/32..25/32, depth 2/16. The old full-height 3/16 boxes were not Beta.
// render_plane re-pinned 2026-10-31 (intended): the wall board is Beta's
// SignModel board (SIGN_WALL_BOARD_*), front face 11/256 off the board
// centre at 1/16 from the wall -> 229/256 or 27/256 (was the old flush
// 1/16 slab's 15/16 or 1/16 face, which the text floated 0.16 in front of).
static const struct wall_facing_equiv wall_facings[] = {
	{SIDE_FRONT, 4, 2, 0, 1,
	 {0.0F, 0.28125F, 0.875F, 1.0F, 0.78125F, 1.0F},
	 SIDE_FRONT, 2, 229.0F / 256.0F, 180},
	{SIDE_BACK, 5, 3, 0, -1,
	 {0.0F, 0.28125F, 0.0F, 1.0F, 0.78125F, 0.125F},
	 SIDE_BACK, 2, 27.0F / 256.0F, 0},
	{SIDE_LEFT, 6, 4, 1, 0,
	 {0.875F, 0.28125F, 0.0F, 1.0F, 0.78125F, 1.0F},
	 SIDE_LEFT, 0, 229.0F / 256.0F, 90},
	{SIDE_RIGHT, 7, 5, -1, 0,
	 {0.0F, 0.28125F, 0.0F, 0.125F, 0.78125F, 1.0F},
	 SIDE_RIGHT, 0, 27.0F / 256.0F, -90},
};

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

static void set(int x, int y, int z, int type, int meta) {
	struct block_data expected = {
		.type = (uint8_t)type,
		.metadata = (uint8_t)meta,
	};
	if(server_world_set_block(&s.worlds[0], x, y, z, expected))
		return;
	struct block_data live;
	assert(server_world_get_block(&s.world, x, y, z, &live));
	assert(live.type == expected.type && live.metadata == expected.metadata);
}

static uint8_t type_at(int x, int y, int z) {
	struct block_data blk;
	assert(server_world_get_block(&s.world, x, y, z, &blk));
	return blk.type;
}

static uint8_t meta_at(int x, int y, int z) {
	struct block_data blk;
	assert(server_world_get_block(&s.world, x, y, z, &blk));
	return blk.metadata;
}

static void assert_box(const struct AABB* box, float x1, float y1, float z1,
					   float x2, float y2, float z2) {
	assert(box->x1 == x1 && box->y1 == y1 && box->z1 == z1);
	assert(box->x2 == x2 && box->y2 == y2 && box->z2 == z2);
}

static float packed_position(const struct displaylist* dl, size_t vertex,
						 int axis) {
	float value;
	memcpy(&value,
		   (const uint8_t*)dl->data + vertex * PC_VERTEX_BYTES + axis * 4,
		   sizeof value);
	return value;
}

static void legacy_wall_text_point(int rotation_deg, float out[3]) {
	const float f = SIGN_TEXT_MODEL_SCALE;
	const float scale = 0.016666668F * f;
	float x = -4.0F * scale;
	float y = 20.0F * scale + 0.5F * f - 0.3125F;
	// 2026-10-31: Beta-exact (TileEntitySignRenderer.java:51,61); the old
	// +0.115 port clearance floated the text off the board on real Wii.
	float z = 0.07F * f - 0.4375F;
	float rx = x;
	float rz = z;
	switch(rotation_deg) {
		case 180: rx = -x; rz = -z; break;
		case 90: rx = -z; rz = x; break;
		case -90: rx = z; rz = -x; break;
		default: assert(rotation_deg == 0); break;
	}
	out[0] = 0.5F + rx;
	out[1] = 0.75F * f + y;
	out[2] = 0.5F + rz;
}

static size_t column_index(int x, int y, int z) {
	return (size_t)x + ((size_t)z + (size_t)y * CHUNK_SIZE) * CHUNK_SIZE;
}

static uint8_t column_metadata_get(const uint8_t* metadata, size_t index) {
	return (metadata[index / 2] >> ((index & 1) * 4)) & 0x0F;
}

static void column_metadata_set(uint8_t* metadata, size_t index,
								uint8_t value) {
	const unsigned shift = (unsigned)(index & 1) * 4;
	metadata[index / 2]
		= (uint8_t)((metadata[index / 2] & ~(0x0FU << shift))
			| ((value & 0x0F) << shift));
}

static void migration_fixture_remove(void) {
	remove(test_fixture_join(MIGRATION_WORLD, "/level.dat"));
	remove(test_fixture_join(MIGRATION_WORLD, "/level.dat_new"));
	remove(test_fixture_join(MIGRATION_WORLD, "/level.dat_old"));
	remove(test_fixture_join(MIGRATION_WORLD, "/region/r.0.0.mcr"));
	RMDIR(test_fixture_join(MIGRATION_WORLD, "/region"));
	RMDIR(MIGRATION_WORLD);
}

enum migration_level_kind {
	MIGRATION_LEVEL_TAGLESS,
	MIGRATION_LEVEL_V1_BETA,
	MIGRATION_LEVEL_V1_CAVEWII,
};

struct migration_strip_tags {
	bool generator;
	bool version;
};

static bool migration_keep_level_tag(const nbt_node* node, void* aux) {
	const struct migration_strip_tags* strip = aux;
	if(!node->name)
		return true;
	if(strip->generator && strcmp(node->name, "CaveWiiGenerator") == 0)
		return false;
	if(strip->version && strcmp(node->name, "CaveWiiSaveVersion") == 0)
		return false;
	return true;
}

static void migration_fixture_write_level(
	string_t world, enum migration_level_kind kind) {
	struct level_archive level;
	assert(level_archive_create_new(
		&level, world, 173, (vec3) {0.5F, 65.62F, 0.5F}));
	nbt_node* version
		= nbt_find_by_path(level.data, LEVEL_CAVEWII_SAVE_VERSION.name);
	assert(version && version->type == TAG_INT);
	version->payload.tag_int = 1;

	struct migration_strip_tags strip = {
		.generator = kind != MIGRATION_LEVEL_V1_CAVEWII,
		.version = kind == MIGRATION_LEVEL_TAGLESS,
	};
	level.data = nbt_filter_inplace(
		level.data, migration_keep_level_tag, &strip);
	assert(level.data);
	FILE* level_file = fopen(test_fixture_join(MIGRATION_WORLD, "/level.dat"), "wb");
	assert(level_file);
	assert(nbt_dump_file(level.data, level_file, STRAT_GZIP) == NBT_OK);
	assert(fclose(level_file) == 0);
	level_archive_discard(&level);
}

static void migration_fixture_create(
	const uint8_t* metas, size_t sign_count, size_t chunk_count,
	enum migration_level_kind kind) {
	assert(metas && sign_count <= 4 && chunk_count > 0
		   && chunk_count <= REGION_SIZE);
	migration_fixture_remove();
	assert(MKDIR(MIGRATION_WORLD) == 0);
	assert(MKDIR(test_fixture_join(MIGRATION_WORLD, "/region")) == 0);
	string_t world;
	string_init_set_str(world, MIGRATION_WORLD);

	struct server_chunk chunk = {0};
	chunk.ids = calloc(COLUMN_BLOCKS, 1);
	chunk.metadata = calloc(COLUMN_BLOCKS / 2, 1);
	chunk.lighting_sky = calloc(COLUMN_BLOCKS / 2, 1);
	chunk.lighting_torch = calloc(COLUMN_BLOCKS / 2, 1);
	chunk.heightmap = calloc(CHUNK_SIZE * CHUNK_SIZE, 1);
	assert(chunk.ids && chunk.metadata && chunk.lighting_sky
		   && chunk.lighting_torch && chunk.heightmap);
	chunk.populated = true;
	for(size_t i = 0; i < sign_count; i++) {
		size_t index = column_index(2 + (int)i * 3, 64, 2);
		chunk.ids[index] = BLOCK_SIGN;
		column_metadata_set(chunk.metadata, index, metas[i]);
	}

	struct region_archive region;
	assert(region_archive_create_new(
		&region, world, 0, 0, WORLD_DIM_OVERWORLD));
	for(size_t chunk_x = 0; chunk_x < chunk_count; chunk_x++) {
		assert(region_archive_set_blocks(
			&region, (w_coord_t)chunk_x, 0, &chunk, NULL));
		if(chunk_x == 0) {
			memset(chunk.ids, 0, COLUMN_BLOCKS);
			memset(chunk.metadata, 0, COLUMN_BLOCKS / 2);
		}
	}
	assert(region_archive_sync(&region));
	assert(region_archive_destroy(&region));
	free(chunk.ids);
	free(chunk.metadata);
	free(chunk.lighting_sky);
	free(chunk.lighting_torch);
	free(chunk.heightmap);
	migration_fixture_write_level(world, kind);
	string_clear(world);
}

static struct buffer migration_fixture_read_region_bytes(void) {
	struct buffer out = {0};
	FILE* file = fopen(
		test_fixture_join(MIGRATION_WORLD, "/region/r.0.0.mcr"), "rb");
	assert(file);
	assert(fseek(file, 0, SEEK_END) == 0);
	long length = ftell(file);
	assert(length >= 0 && fseek(file, 0, SEEK_SET) == 0);
	out.data = malloc((size_t)length);
	out.len = (size_t)length;
	assert(out.data || out.len == 0);
	assert(fread(out.data, 1, out.len, file) == out.len);
	assert(fclose(file) == 0);
	return out;
}

static void migration_chunk_free(struct server_chunk* chunk) {
	mem2_free(chunk->ids);
	mem2_free(chunk->metadata);
	mem2_free(chunk->lighting_sky);
	mem2_free(chunk->lighting_torch);
	mem2_free(chunk->heightmap);
}

static void migration_fixture_assert_walls(
	const uint8_t* expected_metas, const size_t* expected_facings,
	size_t sign_count) {
	string_t world;
	string_init_set_str(world, MIGRATION_WORLD);
	struct region_archive region;
	assert(region_archive_create(
		&region, world, 0, 0, WORLD_DIM_OVERWORLD));
	struct server_chunk loaded = {0};
	assert(region_archive_get_blocks(&region, 0, 0, &loaded, NULL));
	assert(region_archive_destroy(&region));
	for(size_t i = 0; i < sign_count; i++) {
		size_t index = column_index(2 + (int)i * 3, 64, 2);
		assert(loaded.ids[index] == BLOCK_SIGN);
		uint8_t meta = column_metadata_get(loaded.metadata, index);
		assert(meta == expected_metas[i]);
		assert(expected_facings[i]
			   < sizeof wall_facings / sizeof wall_facings[0]);
		const struct wall_facing_equiv* facing
			= wall_facings + expected_facings[i];
		struct block_data wall = {.type = BLOCK_SIGN, .metadata = meta};
		struct block_info info = {.block = &wall};
		struct AABB box;
		assert(block_sign.getBoundingBox(&info, false, &box) == 1);
		assert_box(&box, facing->box[0], facing->box[1], facing->box[2],
				   facing->box[3], facing->box[4], facing->box[5]);
	}
	migration_chunk_free(&loaded);
	string_clear(world);
}

// Reset the MAX_SIGNS free-slot table between cases so an earlier test's
// registration never starves a later one (mirrors server_local_init's boot
// memset; see block_sign.c / server_local.c's menu_dwell_sign_demo comment).
static void reset_sign_slots(void) {
	for(int i = 0; i < MAX_SIGNS; i++)
		s.sign_pos[i].y = -1;
}

// Direct-call harness (the test_hoe.c precedent): drives item_sign's
// onItemPlace exactly like the dispatcher would, without going through the
// SRPC_BLOCK_PLACE plumbing. `wx,wy,wz` = the TARGET cell (already offset by
// on_side, matching what server_local.c's dispatcher precomputes via
// blocks_side_offset); `ox,oy,oz` = the CLICKED cell.
static bool sign_place(int wx, int wy, int wz, int ox, int oy, int oz,
					   enum side on_side) {
	struct block_data where_blk, on_blk;
	assert(server_world_get_block(&s.world, wx, wy, wz, &where_blk));
	assert(server_world_get_block(&s.world, ox, oy, oz, &on_blk));
	struct block_info where = {
		.block = &where_blk, .neighbours = NULL, .x = wx, .y = wy, .z = wz};
	struct block_info on = {
		.block = &on_blk, .neighbours = NULL, .x = ox, .y = oy, .z = oz};
	struct item_data it = {.id = ITEM_SIGN, .durability = 0, .count = 1};
	bool placed = item_sign.onItemPlace(&s, &s.player, &it, &where, &on, on_side);
	// onItemPlace never self-consumes (the block_cake.c precedent) -- the
	// generic dispatch's `if(placed) inventory_consume(...)` owns that,
	// proven separately below via the real SRPC path.
	assert(it.id == ITEM_SIGN && it.count == 1);
	return placed;
}

static void test_registration_bounds_drop_and_predicates(void) {
	// [740c583 Block.java] signPost=63 and signWall=68 are two BlockSign
	// instances with identical material/hardness/sound/drop behavior. This port
	// deliberately shares one struct block implementation between their ids.
	assert(BLOCK_SIGN_POST == 63);
	assert(BLOCK_SIGN == 68);
	assert(blocks[BLOCK_SIGN_POST] == &block_sign);
	assert(blocks[BLOCK_SIGN] == &block_sign);
	assert(block_is_any_sign(BLOCK_SIGN_POST));
	assert(block_is_any_sign(BLOCK_SIGN));
	assert(!block_is_any_sign(BLOCK_STONE));
	assert(piston_mobility_flag(BLOCK_SIGN_POST) == RS_MOBILITY_NORMAL);
	assert(piston_mobility_flag(BLOCK_SIGN) == RS_MOBILITY_NORMAL);

	// [740c583 BlockSign ctor/setBlockBoundsBasedOnState]: the post keeps the
	// ctor box for every 0..15 yaw, including the wall-overlap range 4..7.
	static const uint8_t post_metas[] = {0, 4, 7, 15};
	for(size_t i = 0; i < sizeof post_metas / sizeof post_metas[0]; i++) {
		struct block_data data
			= {.type = BLOCK_SIGN_POST, .metadata = post_metas[i]};
		struct block_info info = {.block = &data};
		struct AABB box;
		assert(block_sign.getBoundingBox(&info, false, &box) == 1);
		assert_box(&box, 0.25F, 0.0F, 0.25F, 0.75F, 1.0F, 0.75F);
		assert(block_sign.getBoundingBox(&info, true, &box) == 0);
	}

	// Beta wall meta 2 keeps the old meta-4 FRONT facing; selection uses
	// BlockSign.setBlockBoundsBasedOnState:38, z=[14/16,1], y=[9/32,25/32].
	struct block_data wall = {.type = BLOCK_SIGN, .metadata = 2};
	struct block_info wall_info = {.block = &wall};
	struct AABB wall_box;
	assert(block_sign.getBoundingBox(&wall_info, false, &wall_box) == 1);
	assert_box(&wall_box, 0.0F, 0.28125F, 0.875F, 1.0F, 0.78125F, 1.0F);
	assert(block_sign.getBoundingBox(&wall_info, true, &wall_box) == 0);

	// [740c583 BlockSign.idDropped] both block ids drop Item.sign (323), never
	// their internal block id.
	struct item_data drop;
	struct block_data post = {.type = BLOCK_SIGN_POST, .metadata = 0};
	struct block_info post_info = {.block = &post};
	assert(block_sign.getDroppedItem(&post_info, &drop, NULL, NULL) == 1);
	assert(drop.id == ITEM_SIGN && drop.count == 1 && drop.durability == 0);
	assert(block_sign.getDroppedItem(&wall_info, &drop, NULL, NULL) == 1);
	assert(drop.id == ITEM_SIGN && drop.count == 1 && drop.durability == 0);

	printf("  ids/properties: shared 63+68 implementation, post bounds, no "
		   "collision, Item.sign drop, shared predicates OK\n");
}

static void test_wall_faces_metadata(void) {
	// [740c583 ItemSign.onItemUse] FRONT/BACK/LEFT/RIGHT -> raw 2/3/4/5.
	reset_sign_slots();
	set(2, 70, 10, BLOCK_STONE, 0);
	set(2, 70, 9, BLOCK_AIR, 0); // FRONT target (-Z)
	assert(sign_place(2, 70, 9, 2, 70, 10, SIDE_FRONT) == true);
	assert(type_at(2, 70, 9) == BLOCK_SIGN);
	assert(meta_at(2, 70, 9) == 2);

	reset_sign_slots();
	set(5, 70, 10, BLOCK_STONE, 0);
	set(5, 70, 11, BLOCK_AIR, 0); // BACK target (+Z)
	assert(sign_place(5, 70, 11, 5, 70, 10, SIDE_BACK) == true);
	assert(type_at(5, 70, 11) == BLOCK_SIGN);
	assert(meta_at(5, 70, 11) == 3);

	reset_sign_slots();
	set(9, 70, 10, BLOCK_STONE, 0);
	set(8, 70, 10, BLOCK_AIR, 0); // LEFT target (-X)
	assert(sign_place(8, 70, 10, 9, 70, 10, SIDE_LEFT) == true);
	assert(type_at(8, 70, 10) == BLOCK_SIGN);
	assert(meta_at(8, 70, 10) == 4);

	reset_sign_slots();
	set(12, 70, 10, BLOCK_STONE, 0);
	set(13, 70, 10, BLOCK_AIR, 0); // RIGHT target (+X)
	assert(sign_place(13, 70, 10, 12, 70, 10, SIDE_RIGHT) == true);
	assert(type_at(13, 70, 10) == BLOCK_SIGN);
	assert(meta_at(13, 70, 10) == 5);

	printf("  wall faces: FRONT/BACK/LEFT/RIGHT -> raw Beta meta 2/3/4/5 OK\n");
}

static void test_wall_metadata_old_new_equivalence(void) {
	uint8_t light[24];
	memset(light, 0xFF, sizeof light);
	uint8_t glyph_width[256];
	memset(glyph_width, 6, sizeof glyph_width);
	glyph_width[(unsigned char)' '] = 4;
	glyph_width[(unsigned char)'X'] = 7;
	char grid[64];
	memset(grid, ' ', sizeof grid);
	grid[0] = 'X';

	for(size_t i = 0; i < sizeof wall_facings / sizeof wall_facings[0]; i++) {
		const struct wall_facing_equiv* facing = wall_facings + i;
		assert(facing->legacy_meta == facing->beta_meta + 2);
		assert(block_sign_wall_metadata_for_side(facing->placement_side)
			== facing->beta_meta);

		struct block_data wall
			= {.type = BLOCK_SIGN, .metadata = facing->beta_meta};
		struct block_info info = {.block = &wall};
		struct AABB box;
		assert(block_sign.getBoundingBox(&info, false, &box) == 1);
		assert_box(&box, facing->box[0], facing->box[1], facing->box[2],
				   facing->box[3], facing->box[4], facing->box[5]);

		// The expected support offset is frozen from the legacy-meta behavior.
		// A present support keeps the sign; removing that exact cell drops it.
		const int x = 3 + (int)i * 6;
		const int y = 82;
		const int z = 24;
		set(x + facing->support_dx, y, z + facing->support_dz,
			BLOCK_STONE, 0);
		set(x, y, z, BLOCK_SIGN, facing->beta_meta);
		struct block_data live;
		assert(server_world_get_block(&s.world, x, y, z, &live));
		struct block_info live_info
			= {.block = &live, .x = x, .y = y, .z = z};
		block_sign.onNeighbourBlockChange(&s, &live_info);
		assert(type_at(x, y, z) == BLOCK_SIGN);
		set(x + facing->support_dx, y, z + facing->support_dz, BLOCK_AIR, 0);
		assert(type_at(x, y, z) == BLOCK_AIR);

		// The front quad stays on the same legacy physical plane.
		info.x = info.y = info.z = 0;
		struct displaylist dl;
		displaylist_init(&dl, 4, 1);
		assert(block_sign.renderBlock(&dl, &info, facing->render_front, NULL,
								  light, false) == 1);
		assert(dl.index == 4 * PC_VERTEX_BYTES);
		for(size_t vertex = 0; vertex < 4; vertex++)
			assert(packed_position(&dl, vertex, facing->render_axis)
				== facing->render_plane);
		displaylist_destroy(&dl);

		// Text rotation is compared with the capture-proven legacy transform,
		// not with another call into the production metadata mapper.
		struct sign_text_vertex verts[SIGN_TEXT_MAX_VERTS];
		assert(sign_text_build_quads(
			grid, facing->beta_meta, true, glyph_width, verts,
			SIGN_TEXT_MAX_VERTS) == 4);
		float expected[3];
		legacy_wall_text_point(facing->text_rotation_deg, expected);
		assert(fabsf(verts[0].x - expected[0]) < 1e-4F);
		assert(fabsf(verts[0].y - expected[1]) < 1e-4F);
		assert(fabsf(verts[0].z - expected[2]) < 1e-4F);
	}

	printf("  equivalence: legacy 4..7 == Beta 2..5 bounds/support/mesh/text for all walls OK\n");
}

static void test_top_face_places_post(void) {
	// [740c583 ItemSign.onItemUse] yaw 90 -> floor((90+180)*16/360
	// +0.5)&15 == 12, placed as signPost id 63.
	reset_sign_slots();
	s.player.rx = 90.0F;
	set(15, 70, 10, BLOCK_STONE, 0);
	set(15, 71, 10, BLOCK_AIR, 0);
	assert(sign_place(15, 71, 10, 15, 70, 10, SIDE_TOP) == true);
	assert(type_at(15, 71, 10) == BLOCK_SIGN_POST);
	assert(meta_at(15, 71, 10) == 12);
	printf("  top face: BLOCK_SIGN_POST meta 12 from yaw 90 OK\n");
}

static void test_support_rules_and_wall_regression(void) {
	// A post whose yaw metadata is 4 is the discriminator regression: the old
	// metadata-bit path treated it as a FRONT wall sign. BlockSign instead
	// selects freestanding support from block id and checks the block below.
	reset_sign_slots();
	s.player.rx = -90.0F; // yaw formula -> metadata 4
	set(2, 75, 20, BLOCK_STONE, 0);
	set(2, 76, 20, BLOCK_AIR, 0);
	assert(sign_place(2, 76, 20, 2, 75, 20, SIDE_TOP));
	assert(type_at(2, 76, 20) == BLOCK_SIGN_POST);
	assert(meta_at(2, 76, 20) == 4);
	set(2, 76, 21, BLOCK_STONE, 0);
	set(2, 76, 21, BLOCK_AIR, 0);
	assert(type_at(2, 76, 20) == BLOCK_SIGN_POST); // side is irrelevant
	set(2, 75, 20, BLOCK_AIR, 0);
	assert(type_at(2, 76, 20) == BLOCK_AIR); // below removed -> drop

	// Beta wall meta 2 remains mounted to z+1. Removing the block below
	// must not affect it; removing its original wall support must drop it.
	reset_sign_slots();
	set(6, 76, 21, BLOCK_STONE, 0); // FRONT/meta-2 support
	set(6, 76, 20, BLOCK_AIR, 0);
	set(6, 75, 20, BLOCK_STONE, 0);
	assert(sign_place(6, 76, 20, 6, 76, 21, SIDE_FRONT));
	assert(type_at(6, 76, 20) == BLOCK_SIGN);
	assert(meta_at(6, 76, 20) == 2);
	set(6, 75, 20, BLOCK_AIR, 0);
	assert(type_at(6, 76, 20) == BLOCK_SIGN);
	set(6, 76, 21, BLOCK_AIR, 0);
	assert(type_at(6, 76, 20) == BLOCK_AIR);

	printf("  support: post checks below by id; wall meta 2 checks z+1 OK\n");
}

static void test_post_is_solid_cactus_neighbour(void) {
	// block_cactus.c's no-collision-but-solid exception must accept BOTH
	// BlockSign ids through block_is_any_sign. Placing a post beside a valid
	// cactus therefore makes the cactus pop, just like a wall sign already did.
	set(10, 74, 20, BLOCK_SAND, 0);
	set(10, 75, 20, BLOCK_CACTUS, 0);
	assert(type_at(10, 75, 20) == BLOCK_CACTUS);
	set(11, 74, 20, BLOCK_STONE, 0);
	set(11, 75, 20, BLOCK_SIGN_POST, 4);
	assert(type_at(10, 75, 20) == BLOCK_AIR);
	assert(type_at(11, 75, 20) == BLOCK_SIGN_POST);

	printf("  shared predicate: post accepted by cactus solid-neighbour path OK\n");
}

static void test_post_render_geometry_and_wall_regression(void) {
	uint8_t light[24];
	memset(light, 0xFF, sizeof light);
	struct block_data post = {
		.type = BLOCK_SIGN_POST, .metadata = 0, .sky_light = 15};
	struct block_info post_info = {
		.block = &post, .x = 8, .y = 70, .z = 24};

	// Every model side has two quads: one board face + one stick face. These
	// fixed-point endpoints are independently pinned to SignModel's quoted
	// addBox values after Beta's 0.6666667 transform (see the test header).
	for(int side = 0; side < SIDE_MAX; side++)
		assert(block_sign.renderBlock(NULL, &post_info, (enum side)side, NULL,
									  light, true) == 2);

	struct displaylist dl;
	displaylist_init(&dl, 8, 1);
	assert(block_sign.renderBlock(&dl, &post_info, SIDE_TOP, NULL, light,
								  false) == 2);
	assert(dl.index == 8 * PC_VERTEX_BYTES);
	const float bx = (float)W2C_COORD(post_info.x);
	const float by = (float)W2C_COORD(post_info.y);
	const float bz = (float)W2C_COORD(post_info.z);
	// Board top: x=[0,256], y=277, z=[117,139] in cell-fixed units.
	assert(packed_position(&dl, 0, 0) == bx + 0.0F / 256.0F);
	assert(packed_position(&dl, 0, 1) == by + 277.0F / 256.0F);
	assert(packed_position(&dl, 0, 2) == bz + 117.0F / 256.0F);
	assert(packed_position(&dl, 2, 0) == bx + 1.0F);
	assert(packed_position(&dl, 2, 2) == bz + 139.0F / 256.0F);
	// Stick top: x/z=[117,139], y=149 in cell-fixed units.
	assert(packed_position(&dl, 4, 0) == bx + 117.0F / 256.0F);
	assert(packed_position(&dl, 4, 1) == by + 149.0F / 256.0F);
	assert(packed_position(&dl, 6, 0) == bx + 139.0F / 256.0F);
	assert(packed_position(&dl, 6, 2) == bz + 139.0F / 256.0F);
	displaylist_destroy(&dl);

	// Metadata 4 overlaps the wall encoding but remains a post: -90-degree
	// model rotation swaps the board's long X axis onto Z.
	post.metadata = 4;
	displaylist_init(&dl, 8, 1);
	assert(block_sign.renderBlock(&dl, &post_info, SIDE_TOP, NULL, light,
								  false) == 2);
	assert(packed_position(&dl, 0, 0) == bx + 139.0F / 256.0F);
	assert(packed_position(&dl, 0, 2) == bz + 0.0F / 256.0F);
	assert(packed_position(&dl, 2, 0) == bx + 117.0F / 256.0F);
	assert(packed_position(&dl, 2, 2) == bz + 1.0F);
	displaylist_destroy(&dl);

	// Block 68 still takes the established one-board-quad wall path for all
	// six sides, including metadata 2.
	struct block_data wall = {.type = BLOCK_SIGN, .metadata = 2};
	struct block_info wall_info = {.block = &wall};
	for(int side = 0; side < SIDE_MAX; side++)
		assert(block_sign.renderBlock(NULL, &wall_info, (enum side)side, NULL,
									  light, true) == 1);

	printf("  render: Beta board+stick fixed geometry rotates; wall path unchanged OK\n");
}

static void test_wall_metadata_provenance_fixtures(void) {
	static const uint8_t beta_metas[] = {2, 3, 4, 5};
	static const uint8_t legacy_metas[] = {4, 5, 6, 7};
	static const size_t all_facings[] = {0, 1, 2, 3};
	static const uint8_t ambiguous_metas[] = {4, 5};
	static const size_t ambiguous_facings[] = {2, 3};
	string_t world;
	string_init_set_str(world, MIGRATION_WORLD);
	struct level_archive level;
	struct wall_sign_metadata_migration_stats stats;
	enum wall_sign_metadata_resolution resolution;

	// (a) A real-Beta level.dat has neither CaveWii tag. Its 2/3 values prove
	// the whole 2..5 set is already Beta-format, so nothing is rewritten.
	migration_fixture_create(
		beta_metas, 4, 1, MIGRATION_LEVEL_TAGLESS);
	assert(level_archive_create(&level, world));
	assert(level_archive_get_save_version(&level) == 0);
	assert(!level_archive_was_created_by_cavewii(&level));
	assert(region_archive_prepare_wall_sign_metadata(
		world, false, false, &resolution, &stats));
	assert(resolution == WALL_SIGN_METADATA_ALREADY_BETA);
	assert(stats.regions_scanned == 1 && stats.chunks_scanned == 1);
	assert(stats.signs_rewritten == 0);
	assert(level_archive_upgrade_save_version(
		&level, CAVEWII_SAVE_VERSION_WALL_SIGNS));
	level_archive_destroy(&level);
	migration_fixture_assert_walls(beta_metas, all_facings, 4);
	migration_fixture_remove();
	printf("  migration fixture a: tagless real-Beta 2/3/4/5 stayed on four physical walls OK\n");

	// (b) Version 1 is only a prior save stamp, not provenance. With the
	// creation-only generator tag absent, the same Beta evidence must win.
	migration_fixture_create(
		beta_metas, 4, 1, MIGRATION_LEVEL_V1_BETA);
	assert(level_archive_create(&level, world));
	assert(level_archive_get_save_version(&level) == 1);
	assert(!level_archive_was_created_by_cavewii(&level));
	assert(region_archive_prepare_wall_sign_metadata(
		world, false, false, &resolution, &stats));
	assert(resolution == WALL_SIGN_METADATA_ALREADY_BETA);
	assert(stats.signs_rewritten == 0);
	assert(level_archive_upgrade_save_version(
		&level, CAVEWII_SAVE_VERSION_WALL_SIGNS));
	level_archive_destroy(&level);
	migration_fixture_assert_walls(beta_metas, all_facings, 4);
	migration_fixture_remove();
	printf("  migration fixture b: save-stamped-v1 real-Beta signs were not converted OK\n");

	// (c) CaveWiiGenerator is installed at creation, so this old 4..7 world is
	// positively owned. Keep the accepted per-chunk crash/retry proof too.
	migration_fixture_create(
		legacy_metas, 4, REGION_SIZE, MIGRATION_LEVEL_V1_CAVEWII);
	assert(level_archive_create(&level, world));
	assert(level_archive_get_save_version(&level) == 1);
	assert(level_archive_was_created_by_cavewii(&level));
	ptime_t started = time_get();
	assert(region_archive_prepare_wall_sign_metadata(
		world, true, false, &resolution, &stats));
	assert(resolution == WALL_SIGN_METADATA_NEEDS_LEGACY_INTENT);
	assert(stats.signs_rewritten == 0);
	assert(level_archive_record_wall_sign_legacy_intent(&level));
	assert(level_archive_has_wall_sign_legacy_intent(&level));
	assert(level_archive_get_save_version(&level) == 1);
	level_archive_discard(&level); // crash/restart after intent, before regions
	assert(level_archive_create(&level, world));
	assert(level_archive_has_wall_sign_legacy_intent(&level));
	assert(level_archive_get_save_version(&level) == 1);
	assert(region_archive_prepare_wall_sign_metadata(
		world, true, true, &resolution, &stats));
	int32_t first_ms = time_diff_ms(started, time_get());
	assert(resolution == WALL_SIGN_METADATA_MIGRATED);
	assert(stats.regions_scanned == 1 && stats.chunks_scanned == REGION_SIZE);
	assert(stats.signs_rewritten == 4);
	assert(region_archive_prepare_wall_sign_metadata(
		world, true, true, &resolution, &stats));
	assert(resolution == WALL_SIGN_METADATA_MIGRATED);
	assert(stats.regions_scanned == 1 && stats.chunks_scanned == REGION_SIZE);
	assert(stats.signs_rewritten == 0);
	assert(level_archive_upgrade_save_version(
		&level, CAVEWII_SAVE_VERSION_WALL_SIGNS));
	level_archive_destroy(&level);
	migration_fixture_assert_walls(beta_metas, all_facings, 4);
	migration_fixture_remove();
	printf("  migration fixture c: CaveWii legacy 4..7 converted + retry-safe on four walls OK (%d ms)\n",
		   first_ms);

	// (d) A complete scan that sees no wall-sign blocks has no metadata to
	// reinterpret. It may stamp only the completed wall-sign schema step; the
	// region archive itself must remain byte-identical.
	static const uint8_t no_sign_metas[] = {0};
	migration_fixture_create(
		no_sign_metas, 0, 1, MIGRATION_LEVEL_TAGLESS);
	struct buffer no_sign_before = migration_fixture_read_region_bytes();
	assert(level_archive_create(&level, world));
	assert(level_archive_get_save_version(&level) == 0);
	assert(region_archive_prepare_wall_sign_metadata(
		world, false, false, &resolution, &stats));
	assert(resolution == WALL_SIGN_METADATA_ALREADY_BETA);
	assert(stats.regions_scanned == 1 && stats.chunks_scanned == 1);
	assert(stats.signs_rewritten == 0);
	assert(level_archive_upgrade_save_version(
		&level, CAVEWII_SAVE_VERSION_WALL_SIGNS));
	level_archive_destroy(&level);
	struct buffer no_sign_after = migration_fixture_read_region_bytes();
	assert(no_sign_after.len == no_sign_before.len);
	assert(memcmp(no_sign_after.data, no_sign_before.data,
				  no_sign_before.len) == 0);
	buffer_free(&no_sign_before);
	buffer_free(&no_sign_after);
	assert(level_archive_create(&level, world));
	assert(level_archive_get_save_version(&level)
		   == CAVEWII_SAVE_VERSION_WALL_SIGNS);
	level_archive_destroy(&level);
	migration_fixture_remove();
	printf("  migration fixture d: tagless no-sign scan stamped v2 with region bytes unchanged OK\n");

	// (e) A current per-chunk marker is not proof that an early-returned chunk
	// contains zero signs. Simulate an interrupted migration whose region record
	// committed but whose durable world-level intent is unavailable; the marker
	// alone must not authorize the no-sign fast path.
	static const uint8_t marked_metas[] = {6};
	migration_fixture_create(
		marked_metas, 1, 1, MIGRATION_LEVEL_TAGLESS);
	assert(region_archive_prepare_wall_sign_metadata(
		world, false, true, &resolution, &stats));
	assert(resolution == WALL_SIGN_METADATA_MIGRATED);
	assert(stats.signs_rewritten == 1);
	assert(region_archive_prepare_wall_sign_metadata(
		world, false, false, &resolution, &stats));
	assert(resolution == WALL_SIGN_METADATA_REFUSED_UNKNOWN);
	migration_fixture_remove();
	printf("  migration fixture e: current chunk marker alone did not authorize v2 OK\n");

	// (f) Raw 4/5 alone is genuinely ambiguous. Refusal is successful and
	// leaves the world writable, the data untouched, and the old version absent
	// even after an ordinary save (which must not manufacture provenance).
	migration_fixture_create(
		ambiguous_metas, 2, 1, MIGRATION_LEVEL_TAGLESS);
	assert(level_archive_create(&level, world));
	assert(region_archive_prepare_wall_sign_metadata(
		world, false, false, &resolution, &stats));
	assert(resolution == WALL_SIGN_METADATA_REFUSED_UNKNOWN);
	assert(stats.signs_rewritten == 0);
	assert(level_archive_get_save_version(&level) == 0);
	int64_t saved_time = 1234;
	assert(level_archive_write(&level, LEVEL_TIME, &saved_time));
	assert(level_archive_save(&level));
	assert(level_archive_get_save_version(&level) == 0);
	assert(!nbt_find_by_path(
		level.data, LEVEL_CAVEWII_SAVE_VERSION.name));
	assert(!level_archive_has_wall_sign_legacy_intent(&level));
	level_archive_destroy(&level);
	assert(level_archive_create(&level, world));
	int64_t loaded_time = 0;
	assert(level_archive_read(&level, LEVEL_TIME, &loaded_time, 0));
	assert(loaded_time == saved_time);
	assert(level_archive_get_save_version(&level) == 0);
	level_archive_destroy(&level);
	migration_fixture_assert_walls(
		ambiguous_metas, ambiguous_facings, 2);
	migration_fixture_remove();
	printf("  migration fixture f: ambiguous 4/5 refused intact; world remained loadable+writable OK\n");

	string_clear(world);
}

static void test_bottom_face_refused(void) {
	// [740c583 ItemSign.onItemUse] `if (var7 == 0) return false;`
	reset_sign_slots();
	set(17, 71, 10, BLOCK_STONE, 0);
	set(17, 70, 10, BLOCK_AIR, 0);
	assert(sign_place(17, 70, 10, 17, 71, 10, SIDE_BOTTOM) == false);
	assert(type_at(17, 70, 10) == BLOCK_AIR);
	printf("  bottom face: refused OK\n");
}

static void test_nonsolid_clicked_block_refused(void) {
	// [740c583] `!var3.getBlockMaterial(var4,var5,var6).isSolid()` -- tall
	// grass is not solid.
	reset_sign_slots();
	set(20, 70, 10, BLOCK_TALL_GRASS, 1);
	set(19, 70, 10, BLOCK_AIR, 0);
	assert(sign_place(19, 70, 10, 20, 70, 10, SIDE_LEFT) == false);
	assert(type_at(19, 70, 10) == BLOCK_AIR);
	printf("  non-solid clicked block (tall grass): refused OK\n");
}

static void test_target_not_replaceable_refused(void) {
	// Target already occupied by a solid, non-place_ignore block.
	reset_sign_slots();
	set(24, 70, 10, BLOCK_STONE, 0);
	set(23, 70, 10, BLOCK_STONE, 0); // target NOT replaceable
	assert(sign_place(23, 70, 10, 24, 70, 10, SIDE_LEFT) == false);
	assert(type_at(23, 70, 10) == BLOCK_STONE);
	printf("  non-replaceable target: refused OK\n");
}

static void test_yaw_post_direction_formula(void) {
	// [740c583 ItemSign.onItemUse / MathHelper.floor_double] MathHelper.
	// floor_double((yaw+180)*16/360 + 0.5) & 15. All these land on an exact
	// X.5 boundary in float/double (16/360 divides evenly for these angles),
	// so there is no float-rounding ambiguity to worry about.
	assert(item_sign_post_direction_from_yaw(0.0F) == 8);
	assert(item_sign_post_direction_from_yaw(45.0F) == 10);
	assert(item_sign_post_direction_from_yaw(90.0F) == 12);
	assert(item_sign_post_direction_from_yaw(180.0F) == 0);  // 16 & 15 == 0
	assert(item_sign_post_direction_from_yaw(-90.0F) == 4);
	assert(item_sign_post_direction_from_yaw(225.0F) == 2);  // 18 & 15 == 2
	assert(item_sign_post_direction_from_yaw(270.0F) == 4);  // 20 & 15 == 4
	assert(item_sign_post_direction_from_yaw(315.0F) == 6);  // 22 & 15 == 6

	// Negative-v case: proves floor_double's floor-toward-negative-infinity,
	// NOT a naive truncating (int) cast. yaw=-200 -> v = (-20*16/360)+0.5 =
	// -0.8888...+0.5 = -0.3888...; floor(-0.3888...) == -1, and -1 & 15 == 15
	// (two's complement -- a naive (int) truncation would instead give 0 & 15
	// == 0, the WRONG answer this golden pins against).
	assert(item_sign_post_direction_from_yaw(-200.0F) == 15);

	printf("  yaw -> signPost 16-direction formula pinned (incl. a negative-v "
		  "floor_double case) OK\n");
}

static void test_dig_time_beta_hardness(void) {
	// [740c583 Block.java:655,660] signPost (63) and signWall (68) both
	// setHardness(1.0F); field scale = beta_hardness * 1500 (derivation in
	// test_redstone_ore_dig.c) -> 1500 ms bare-hand. Signs are absent from
	// ItemAxe.blocksEffectiveAgainst {planks, bookShelf, wood, chest} and
	// MATERIAL_WOOD needs no tool, so an axe digs NO faster than a hand --
	// unlike planks, where the axe tier divides. One shared struct covers
	// both block ids. (Pinned against the old 4500 "TODO" value, which was
	// 3x Beta's break time.)
	struct item_data d = {.id = ITEM_IRON_AXE, .count = 1};
	struct item* axe = item_get(&d);
	assert(axe && "iron axe not registered");
	assert(tool_dig_delay_ms(&block_sign, NULL) == 1500);
	assert(tool_dig_delay_ms(&block_sign, axe) == 1500);

	printf("  dig time: 1500ms bare-hand AND iron axe (no axe bonus) OK\n");
}

static void test_dispatch_wall_consume_and_gui_open(void) {
	// Full SRPC_BLOCK_PLACE path (the test_bucket.c production_use_dispatch
	// precedent): proves the item's stack is consumed ONLY on a successful
	// placement (Beta `--stackSize`, auto-consumed by the generic dispatch,
	// NOT by onItemPlace itself) and that the sign edit GUI opens immediately
	// after (Beta `player.displayGUIEditSign(te)`).
	reset_sign_slots();
	const int x = 27, y = 70, z = 10;
	set(x, y, z, BLOCK_STONE, 0);
	set(x, y, z - 1, BLOCK_AIR, 0); // FRONT target

	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR + 0,
					   (struct item_data) {.id = ITEM_SIGN, .count = 1});
	inventory_set_hotbar(&s.player.inventory, 0);
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.x = x + 0.5;
	s.player.y = y + 1.62;
	s.player.z = z + 3.5; // well clear of the target cell

	struct server_rpc use = {
		.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = SIDE_FRONT},
	};
	server_local_test_process_rpc(&s, &use);

	assert(type_at(x, y, z - 1) == BLOCK_SIGN);
	assert(meta_at(x, y, z - 1) == 2); // Beta FRONT

	struct item_data held;
	bool has_held = inventory_get_slot(&s.player.inventory,
									   INVENTORY_SLOT_HOTBAR + 0, &held);
	// A max_stack==1 item exhausted to 0 clears the slot entirely (this
	// engine's convention) -- either an empty slot or an explicit count==0
	// both mean "consumed".
	assert(!has_held || held.count == 0);

	assert(s.player.active_inventory != &s.player.inventory);
	assert(s.player.active_inventory
		  && s.player.active_inventory->logic == &inventory_logic_sign);
	assert(server_local_test_close_active_inventory(&s));

	printf("  SRPC_BLOCK_PLACE wall: placed, stack consumed, edit GUI opened OK\n");
}

static void test_dispatch_post_consume_and_gui_open(void) {
	// The same full dispatcher tail must accept id 63 everywhere it validates
	// sign backing (inventory_sign.c and server_local.c), consume Item.sign,
	// and leave the new sign OSK closeable. yaw 45 -> metadata 10.
	reset_sign_slots();
	const int x = 4, y = 80, z = 28;
	set(x, y, z, BLOCK_STONE, 0);
	set(x, y + 1, z, BLOCK_AIR, 0);

	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR + 0,
					   (struct item_data) {.id = ITEM_SIGN, .count = 1});
	inventory_set_hotbar(&s.player.inventory, 0);
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.rx = 45.0F;
	s.player.x = x + 0.5;
	s.player.y = y + 1.62;
	s.player.z = z + 3.5;

	struct server_rpc use = {
		.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = SIDE_TOP},
	};
	server_local_test_process_rpc(&s, &use);

	assert(type_at(x, y + 1, z) == BLOCK_SIGN_POST);
	assert(meta_at(x, y + 1, z) == 10);
	struct item_data held;
	assert(!inventory_get_slot(&s.player.inventory,
							   INVENTORY_SLOT_HOTBAR + 0, &held)
		   || held.count == 0);
	assert(s.player.active_inventory
		  && s.player.active_inventory->logic == &inventory_logic_sign);
	assert(server_local_test_close_active_inventory(&s));
	assert(s.player.active_inventory == &s.player.inventory);

	printf("  SRPC_BLOCK_PLACE post: id 63 consumed, OSK opened + closed OK\n");
}

static void test_dispatch_refused_keeps_stack(void) {
	// The mirror case: a refused placement (bottom face) must NOT consume the
	// stack and must NOT open any GUI.
	reset_sign_slots();
	const int x = 30, y = 71, z = 10;
	set(x, y, z, BLOCK_STONE, 0);
	set(x, y - 1, z, BLOCK_AIR, 0);

	inventory_set_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR + 0,
					   (struct item_data) {.id = ITEM_SIGN, .count = 1});
	inventory_set_hotbar(&s.player.inventory, 0);
	s.player.has_pos = true;
	s.player.finished_loading = true;
	s.player.health = MAX_PLAYER_HEALTH;
	s.player.x = x + 0.5;
	s.player.y = y + 1.62;
	s.player.z = z + 3.5;

	struct server_rpc use = {
		.type = SRPC_BLOCK_PLACE,
		.payload.block_place = {.x = x, .y = y, .z = z, .side = SIDE_BOTTOM},
	};
	server_local_test_process_rpc(&s, &use);

	assert(type_at(x, y - 1, z) == BLOCK_AIR);
	struct item_data held;
	assert(inventory_get_slot(&s.player.inventory, INVENTORY_SLOT_HOTBAR + 0,
							  &held));
	assert(held.id == ITEM_SIGN && held.count == 1); // untouched
	assert(s.player.active_inventory == &s.player.inventory); // no GUI opened

	printf("  SRPC_BLOCK_PLACE bottom face: refused, stack + GUI untouched OK\n");
}

int main(void) {
	test_fixture_dir("test_sign_placement_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	test_fixture_dir("test_sign_meta_migration_tmp", s_fixture_MIGRATION_WORLD, sizeof s_fixture_MIGRATION_WORLD);
	clin_init();
	struct thread drain;
	thread_create(&drain, drain_entry, NULL, 64);
	blocks_init();
	items_init();
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // repeatable clean-slate fixture
	string_t level;
	string_init_set_str(level, TMP_WORLD);
	memset(&s, 0, sizeof(s));
	if(!server_local_te_tables_init(&s)) /* IMP-TE-CAPS: growable slabs */
		abort();
	server_world_create(&s.world, level, WORLD_DIM_OVERWORLD, 4242,
						true /*superflat*/);
	s.worlds[0].session = &s; /* A1a (D1): the owning session (callbacks, services); not the archive binding `local` */
	s.scheduled_tick_budget = 0;
	rand_gen_seed(&s.rand_src);
	dict_entity_init(s.entities);
	reset_sign_slots();
	struct server_chunk* sc = NULL;
	for(int cx = 0; cx <= 1; cx++)
		for(int cz = 0; cz <= 1; cz++)
			assert(server_world_load_chunk(&s.world, cx, cz, &sc) && sc);

	assert(inventory_create(&s.player.inventory, &inventory_logic_player, &s,
							&s.player, INVENTORY_SIZE, 0, 0, 0));
	s.player.active_inventory = &s.player.inventory;

	printf("test_sign_placement:\n");
	test_registration_bounds_drop_and_predicates();
	test_wall_faces_metadata();
	test_wall_metadata_old_new_equivalence();
	test_top_face_places_post();
	test_support_rules_and_wall_regression();
	test_post_is_solid_cactus_neighbour();
	test_post_render_geometry_and_wall_regression();
	test_wall_metadata_provenance_fixtures();
	test_bottom_face_refused();
	test_nonsolid_clicked_block_refused();
	test_target_not_replaceable_refused();
	test_yaw_post_direction_formula();
	test_dig_time_beta_hardness();
	test_dispatch_wall_consume_and_gui_open();
	test_dispatch_post_consume_and_gui_open();
	test_dispatch_refused_keeps_stack();

	g_drain_run = 0;
	thread_join(&drain);
	printf("PASS test_sign_placement\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	test_fixture_dir_cleanup(MIGRATION_WORLD);
	return 0;
}
