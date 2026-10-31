/*
	FH-07 save-version-3 migration test: the fork's iron chest moves off Beta's
	id 95 (BlockLockedChest) to the net-new 200, per-instance provenance-proven.

	The load-bearing fact (verified in production code, cited in
	region_archive.c's migration header): every PLACED iron chest owns a
	chest-store row from the placement handler onward, and the region writer
	emits every row as a Chest TileEntity compound even with an empty Items
	list -- while Beta's BlockLockedChest extends Block, not BlockContainer,
	and NEVER has a tile entity. So "95 + Chest TE at the coordinate" proves
	the port's iron chest; "95 without one" is a genuine locked chest.

	Fixture chunk (single region r.0.0.mcr, chunk 0,0):
	  (2,64,2)  block 95 WITH a Chest TE holding 3 cobblestone  -> iron chest
	  (5,64,5)  block 95 with NO TE                             -> locked chest
	  (8,64,8)  block 54 (wooden chest) whose Items holds an id-95 ITEM stack
	            (an iron chest item stored inside a wooden chest)

	Proven, in order:
	  1) CaveWii-provenance mode (rewrite_unproven_blocks + rewrite_item_ids,
	     what server_local passes for a generator-tagged world): BOTH 95 blocks
	     become 200, the stored item id becomes 200, the iron chest's own TE
	     (and its contents) survives untouched at the same coordinate.
	  2) Instance-proof mode (both flags false, the Beta-import posture): ONLY
	     the TE-proven 95 converts; the bare 95 and the stored item stay -- a
	     real Beta world's locked chests are never rewritten by evidence-free
	     sweeps.
	  3) The two-pass evidence escalation server_local performs for pre-tag
	     CaveWii worlds: pass 1 (false,false) found a proven iron chest, so
	     pass 2 (false,true) is authorized and rewrites the stored item id.
	  4) Idempotency: re-running the full rewrite reports zero further work --
	     the crash-retry story is "just run it again", no journal needed.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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
#include "block/blocks_data.h"
#include "network/region_archive.h"
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"
#include "platform/mem2.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_iron_chest_migrate_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define COLUMN_BLOCKS ((size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)

// Same index layout as region_archive.c / server_world.c:
// index = y + (z + x*CHUNK_SIZE) * WORLD_HEIGHT.
static size_t column_index(int x, int y, int z) {
	return (size_t)y + ((size_t)z + (size_t)x * CHUNK_SIZE) * WORLD_HEIGHT;
}

static void fixture_remove(void) {
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	RMDIR(test_fixture_join(TMP_WORLD, "/region"));
	RMDIR(TMP_WORLD);
}

// The pre-migration id both chests were saved under: Beta's lockedChest slot,
// which the fork's iron chest squatted. Spelled as a literal on purpose -- the
// fixture models OLD saves, whose bytes do not change when enums do.
#define OLD_IRON_CHEST_ID 95

static void fixture_create(void) {
	fixture_remove();
	assert(MKDIR(TMP_WORLD) == 0);
	assert(MKDIR(test_fixture_join(TMP_WORLD, "/region")) == 0);
	string_t world;
	string_init_set_str(world, TMP_WORLD);

	struct server_chunk chunk = {0};
	chunk.ids = calloc(COLUMN_BLOCKS, 1);
	chunk.metadata = calloc(COLUMN_BLOCKS / 2, 1);
	chunk.lighting_sky = calloc(COLUMN_BLOCKS / 2, 1);
	chunk.lighting_torch = calloc(COLUMN_BLOCKS / 2, 1);
	chunk.heightmap = calloc(CHUNK_SIZE * CHUNK_SIZE, 1);
	assert(chunk.ids && chunk.metadata && chunk.lighting_sky
		   && chunk.lighting_torch && chunk.heightmap);
	chunk.populated = true;

	chunk.ids[column_index(2, 64, 2)] = OLD_IRON_CHEST_ID; // TE-proven
	chunk.ids[column_index(5, 64, 5)] = OLD_IRON_CHEST_ID; // bare
	chunk.ids[column_index(8, 64, 8)] = BLOCK_CHEST;	   // holds the 95 ITEM

	struct te_chest chests[2];
	memset(chests, 0, sizeof(chests));
	chests[0].x = 2;
	chests[0].y = 64;
	chests[0].z = 2;
	chests[0].items[0]
		= (struct item_data) {.id = 4 /*cobblestone*/, .count = 3};
	chests[1].x = 8;
	chests[1].y = 64;
	chests[1].z = 8;
	chests[1].items[0]
		= (struct item_data) {.id = OLD_IRON_CHEST_ID, .count = 1};
	struct chunk_tile_entities te = {0};
	te.chests = chests;
	te.n_chest = 2;

	struct region_archive region;
	assert(region_archive_create_new(
		&region, world, 0, 0, WORLD_DIM_OVERWORLD));
	assert(region_archive_set_blocks(&region, 0, 0, &chunk, &te));
	assert(region_archive_sync(&region));
	assert(region_archive_destroy(&region));

	free(chunk.ids);
	free(chunk.metadata);
	free(chunk.lighting_sky);
	free(chunk.lighting_torch);
	free(chunk.heightmap);
	string_clear(world);
}

struct chunk_state {
	uint8_t id_proven;	 // block at (2,64,2)
	uint8_t id_bare;	 // block at (5,64,5)
	uint8_t id_wooden;	 // block at (8,64,8)
	int n_chest;
	uint16_t stored_item_id; // wooden chest slot 0
	uint8_t stored_count;
	uint16_t iron_item_id; // iron chest slot 0 (the cobblestone)
	uint8_t iron_count;
	bool iron_te_present;
};

static void read_state(struct chunk_state* out) {
	string_t world;
	string_init_set_str(world, TMP_WORLD);
	struct region_archive region;
	assert(region_archive_create(&region, world, 0, 0, WORLD_DIM_OVERWORLD));
	struct server_chunk loaded = {0};
	struct chunk_tile_entities te = {0};
	assert(region_archive_get_blocks(&region, 0, 0, &loaded, &te));
	assert(region_archive_destroy(&region));

	memset(out, 0, sizeof(*out));
	out->id_proven = loaded.ids[column_index(2, 64, 2)];
	out->id_bare = loaded.ids[column_index(5, 64, 5)];
	out->id_wooden = loaded.ids[column_index(8, 64, 8)];
	out->n_chest = te.n_chest;
	for(int i = 0; i < te.n_chest; i++) {
		if(te.chests[i].x == 8 && te.chests[i].y == 64
		   && te.chests[i].z == 8) {
			out->stored_item_id = te.chests[i].items[0].id;
			out->stored_count = te.chests[i].items[0].count;
		}
		if(te.chests[i].x == 2 && te.chests[i].y == 64
		   && te.chests[i].z == 2) {
			out->iron_te_present = true;
			out->iron_item_id = te.chests[i].items[0].id;
			out->iron_count = te.chests[i].items[0].count;
		}
	}
	tile_entities_parse_free(&te);
	server_chunk_arrays_free(&loaded);
	string_clear(world);
}

int main(void) {
	test_fixture_dir("test_iron_chest_migrate_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	string_t world;
	string_init_set_str(world, TMP_WORLD);
	struct iron_chest_migration_stats stats;

	// --- 1) CaveWii-provenance mode: everything converts ------------------
	fixture_create();
	assert(region_archive_migrate_iron_chest_ids(world, true, true, &stats));
	assert(stats.regions_scanned == 1);
	assert(stats.chunks_scanned == 1);
	assert(stats.blocks_rewritten == 2); // proven AND bare
	assert(stats.items_rewritten == 1);	 // the stored iron chest item
	struct chunk_state st;
	read_state(&st);
	assert(st.id_proven == BLOCK_IRON_CHEST); // 200
	assert(st.id_bare == BLOCK_IRON_CHEST);
	assert(st.id_wooden == BLOCK_CHEST); // untouched
	assert(st.n_chest == 2);			 // no TE gained or lost
	assert(st.iron_te_present);			 // contents survived, same coordinate
	assert(st.iron_item_id == 4 && st.iron_count == 3);
	assert(st.stored_item_id == BLOCK_IRON_CHEST && st.stored_count == 1);
	printf("[1] cavewii-provenance full rewrite OK\n");

	// --- 4a) Idempotency on the migrated world ----------------------------
	assert(region_archive_migrate_iron_chest_ids(world, true, true, &stats));
	assert(stats.blocks_rewritten == 0 && stats.items_rewritten == 0);
	printf("[4a] idempotent re-run is a no-op\n");

	// --- 2) Instance-proof mode: only the TE-proven block converts --------
	fixture_create();
	assert(region_archive_migrate_iron_chest_ids(world, false, false, &stats));
	assert(stats.blocks_rewritten == 1); // ONLY the proven one
	assert(stats.items_rewritten == 0);	 // items pass not authorized
	read_state(&st);
	assert(st.id_proven == BLOCK_IRON_CHEST);
	assert(st.id_bare == OLD_IRON_CHEST_ID); // a genuine locked chest survives
	assert(st.stored_item_id == OLD_IRON_CHEST_ID); // item untouched
	assert(st.iron_item_id == 4 && st.iron_count == 3);
	printf("[2] instance-proof-only rewrite OK (bare 95 preserved)\n");

	// --- 3) Two-pass evidence escalation (pre-tag CaveWii world) ----------
	// Pass 1 above proved the world contains port iron chests
	// (blocks_rewritten > 0), which authorizes the items pass server_local
	// runs next -- exactly this call:
	assert(region_archive_migrate_iron_chest_ids(world, false, true, &stats));
	assert(stats.items_rewritten == 1);
	read_state(&st);
	assert(st.stored_item_id == BLOCK_IRON_CHEST);
	assert(st.id_bare == OLD_IRON_CHEST_ID); // STILL never converted
	printf("[3] two-pass evidence escalation OK\n");

	// --- 4b) Idempotency again on the mixed-outcome world -----------------
	assert(region_archive_migrate_iron_chest_ids(world, false, true, &stats));
	assert(stats.blocks_rewritten == 0 && stats.items_rewritten == 0);
	printf("[4b] idempotent on the mixed world too\n");

	fixture_remove();
	string_clear(world);
	printf("test_iron_chest_migrate: ALL PASS\n");
	return 0;
}
