/*
	Full region-file round-trip for chest/sign tile entities through the REAL
	persistence path: region_archive_set_blocks -> an .mcr file on disk (whole
	chunk NBT + zlib) -> region_archive_get_blocks. Stage 2 of the chest-store
	redesign (docs/chest_store_redesign.md).

	This is the corruption check the design doc's risk note demands. set_blocks
	rewrites the WHOLE chunk record, so the assertion that matters is not just
	that the chests round-trip, but that the chunk's TERRAIN
	(Blocks/Data/SkyLight/BlockLight/HeightMap/TerrainPopulated) survives a write
	that *also* carries a populated TileEntities list. Both are checked, byte for
	byte, against a known input pattern.
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
#include "network/region_archive.h"
#include "network/server_world.h" // struct server_chunk, CHUNK_SIZE, WORLD_HEIGHT
#include "network/tile_entities_nbt.h"
#include "platform/mem2.h"

#define IDS_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)
#define NIB_SIZE (CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT / 2)
#define HM_SIZE	 (CHUNK_SIZE * CHUNK_SIZE)
static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_region_te_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD
#define TEST_CHUNK_X 1
#define TEST_CHUNK_Z 1

int main(void) {
	test_fixture_dir("test_region_te_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	// 1) Throwaway world dir with the region/ subfolder region_archive expects.
	MKDIR(TMP_WORLD);
	MKDIR(test_fixture_join(TMP_WORLD, "/region"));
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr")); // start from a clean region file

	string_t level;
	string_init_set_str(level, TMP_WORLD);

	struct region_archive ra;
	bool ok = region_archive_create_new(&ra, level, 0, 0, WORLD_DIM_OVERWORLD);
	assert(ok);

	// 2) A chunk with a recognizable, position-dependent terrain pattern.
	struct server_chunk sc = {0};
	sc.ids = malloc(IDS_SIZE);
	sc.metadata = malloc(NIB_SIZE);
	sc.lighting_sky = malloc(NIB_SIZE);
	sc.lighting_torch = malloc(NIB_SIZE);
	sc.heightmap = malloc(HM_SIZE);
	assert(sc.ids && sc.metadata && sc.lighting_sky && sc.lighting_torch
		   && sc.heightmap);
	for(int i = 0; i < IDS_SIZE; i++)
		sc.ids[i] = (uint8_t)(i * 7 + 3);
	for(int i = 0; i < NIB_SIZE; i++) {
		sc.metadata[i] = (uint8_t)(i * 5 + 1);
		sc.lighting_sky[i] = (uint8_t)(i * 11 + 2);
		sc.lighting_torch[i] = (uint8_t)(i * 13 + 4);
	}
	for(int i = 0; i < HM_SIZE; i++)
		sc.heightmap[i] = (uint8_t)(i + 64);
	sc.modified = true;
	sc.populated = true;

	// 3) Chests + a sign, all inside non-origin chunk (1,1). This makes the
	// region integration prove that it forwards the actual owning coordinates.
	struct te_chest chests[2];
	memset(chests, 0, sizeof(chests));
	chests[0] = (struct te_chest) {.x = 19, .y = 70, .z = 21};
	chests[0].items[0] = (struct item_data) {.id = 265, .count = 7};
	chests[0].items[26] = (struct item_data) {.id = 331, .count = 64};
	chests[1] = (struct te_chest) {.x = 24, .y = 12, .z = 27};
	chests[1].items[53] = (struct item_data) {.id = 351, .count = 1, .durability = 3};
	// A part-worn DIAMOND pickaxe (id 278, max_damage 1562): durability 1500
	// does not fit in 8 bits -- the old uint8 item_data field truncated it to
	// 220 through this exact NBT path (Damage is a SHORT in Beta saves). Pins
	// the 2026-06-11 widening; would fail mod-256 on any regression.
	chests[1].items[0] = (struct item_data) {.id = 278, .count = 1, .durability = 1500};
	struct te_sign signs[1];
	memset(signs, 0, sizeof(signs));
	signs[0] = (struct te_sign) {.x = 17, .y = 65, .z = 18};
	strcpy(signs[0].text, "a saved sign");
	// A mid-smelt furnace: input + fuel + partial output, non-zero BurnTime/CookTime.
	struct te_furnace furnaces[1];
	memset(furnaces, 0, sizeof(furnaces));
	furnaces[0] = (struct te_furnace) {.x = 22, .y = 64, .z = 25};
	furnaces[0].items[0] = (struct item_data) {.id = 15, .count = 8};	// input
	furnaces[0].items[1] = (struct item_data) {.id = 263, .count = 3}; // fuel
	furnaces[0].items[2] = (struct item_data) {.id = 265, .count = 4}; // output
	furnaces[0].burn_time = 412;
	furnaces[0].cook_time = 173;
	struct te_piston_moving pistons[1] = {{
		.x = 31,
		.y = 127,
		.z = 16,
		.stored_id = 255,
		.stored_meta = 15,
		.facing = 5,
		.progress = 0.5F,
		.extending = 1,
	}};
	// A stocked dispenser (dispenser-slice): its records were absent from this
	// round-trip until the D12 sweep, which is exactly how the writer's
	// kind-enumeration drift stayed invisible.
	struct te_dispenser dispensers[1];
	memset(dispensers, 0, sizeof(dispensers));
	dispensers[0] = (struct te_dispenser) {.x = 26, .y = 68, .z = 20};
	dispensers[0].items[0] = (struct item_data) {.id = 262, .count = 5};
	dispensers[0].items[8] = (struct item_data) {.id = 332, .count = 16};
	// A note block (noteblock-slice) and a jukebox with a record inserted
	// (jukebox-slice): the SAME D12 shape as the dispenser above -- a kind
	// missing from tile_entities_any() would silently lose a contents-only
	// edit on eviction, so both must round-trip through the real .mcr path.
	struct te_note notes[1];
	memset(notes, 0, sizeof(notes));
	notes[0] = (struct te_note) {.x = 18, .y = 66, .z = 24, .note = 17};
	struct te_record_player record_players[1];
	memset(record_players, 0, sizeof(record_players));
	record_players[0]
		= (struct te_record_player) {.x = 29, .y = 69, .z = 19, .record = 2257};
	struct chunk_tile_entities te = {
		.chests = chests,
		.n_chest = 2,
		.signs = signs,
		.n_sign = 1,
		.furnaces = furnaces,
		.n_furnace = 1,
		.dispensers = dispensers,
		.n_dispenser = 1,
		.notes = notes,
		.n_note = 1,
		.record_players = record_players,
		.n_record_player = 1,
		.piston_movings = pistons,
		.n_piston_moving = 1,
	};

	// 4) Write the chunk (terrain + tile entities) to the real region file.
	ok = region_archive_set_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &sc, &te);
	assert(ok);
	// Save-side ownership and persisted-range validation is transactional: none
	// of these malformed records may replace the already committed chunk.
	chests[0].x = 32;
	assert(!region_archive_set_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &sc, &te));
	chests[0].x = 19;
	signs[0].y = 128;
	assert(!region_archive_set_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &sc, &te));
	signs[0].y = 65;
	furnaces[0].z = 15;
	assert(!region_archive_set_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &sc, &te));
	furnaces[0].z = 25;
	pistons[0].x = 15;
	assert(!region_archive_set_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &sc, &te));
	pistons[0].x = 31;
	pistons[0].stored_meta = 16;
	assert(!region_archive_set_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &sc, &te));
	pistons[0].stored_meta = 15;
	dispensers[0].x = 33; // outside chunk (1,1): the whole write must reject
	assert(!region_archive_set_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &sc, &te));
	dispensers[0].x = 26;
	notes[0].x = 40; // outside chunk (1,1): the whole write must reject
	assert(!region_archive_set_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &sc, &te));
	notes[0].x = 18;
	record_players[0].z = 40; // outside chunk (1,1): the whole write must reject
	assert(!region_archive_set_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &sc, &te));
	record_players[0].z = 19;
	// A non-empty codec build that runs out of memory must abort before replacing
	// the committed chunk. The following successful read proves the prior record
	// remained intact.
	tile_entities_nbt_test_fail_allocation_after(0);
	assert(!region_archive_set_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &sc, &te));
	// D12 fail-open guard: when the ONLY nonempty kind is the dispenser list,
	// the writer's "anything to persist?" check used to enumerate
	// chest/sign/furnace/piston and miss it -- this exact OOM was then treated
	// as "genuinely empty" and COMMITTED an empty TileEntities list, silently
	// wiping the dispenser contents from disk. It must abort like the
	// mixed-kind case above.
	struct chunk_tile_entities te_disp_only
		= {.dispensers = dispensers, .n_dispenser = 1};
	tile_entities_nbt_test_fail_allocation_after(0);
	assert(!region_archive_set_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &sc, &te_disp_only));
	// Same D12 fail-open guard, note-only (noteblock-slice): tile_entities_
	// any() must count n_note too, or a note-only chunk's failed NBT build
	// would be treated as "genuinely empty" and committed with an empty list.
	struct chunk_tile_entities te_note_only
		= {.notes = notes, .n_note = 1};
	tile_entities_nbt_test_fail_allocation_after(0);
	assert(!region_archive_set_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &sc, &te_note_only));
	// Same guard, record_player-only (jukebox-slice).
	struct chunk_tile_entities te_record_player_only
		= {.record_players = record_players, .n_record_player = 1};
	tile_entities_nbt_test_fail_allocation_after(0);
	assert(!region_archive_set_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &sc, &te_record_player_only));

	// 5) Read it back through the real path -- and prove the MEM2 ROUTING
	// INVARIANT for the disk path: the five resident arrays must come from
	// mem2_alloc, NOT a borrow of cNBT's plain-malloc (= MEM1 on Wii) payloads.
	// The old borrow pinned ~82KB of MEM1 per disk-loaded column and re-created
	// the P0 exhaustion the worldgen path was fixed for (the real-Wii
	// 2026-06-07 watchdog crash: "heap used 6751KB free 612KB"). The mem2 stat
	// counters only tick for mem2_*-routed memory, so before the copy-out fix
	// this delta was 0 -- the structural blind spot made visible.
	size_t reload_alloc0 = mem2_stat_alloc_calls();
	size_t reload_bytes0 = mem2_stat_alloc_bytes();
	struct server_chunk sc2 = {0};
	struct chunk_tile_entities te_out = {0};
	ok = region_archive_get_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &sc2, &te_out);
	assert(ok);
	assert(mem2_stat_alloc_calls() - reload_alloc0 >= 5);
	assert(mem2_stat_alloc_bytes() - reload_bytes0
		   >= (size_t)IDS_SIZE + 3 * NIB_SIZE + HM_SIZE);

	// 6) TERRAIN must be byte-identical -- the whole point of the risk note:
	// a tile-entity construction bug must not corrupt the chunk's blocks.
	assert(memcmp(sc2.ids, sc.ids, IDS_SIZE) == 0);
	assert(memcmp(sc2.metadata, sc.metadata, NIB_SIZE) == 0);
	assert(memcmp(sc2.lighting_sky, sc.lighting_sky, NIB_SIZE) == 0);
	assert(memcmp(sc2.lighting_torch, sc.lighting_torch, NIB_SIZE) == 0);
	assert(memcmp(sc2.heightmap, sc.heightmap, HM_SIZE) == 0);
	assert(sc2.populated == true);

	// 7) ...and the tile entities survive (build/parse preserve list order).
	assert(te_out.n_chest == 2 && te_out.n_sign == 1);
	assert(te_out.chests[0].x == 19 && te_out.chests[0].y == 70
		   && te_out.chests[0].z == 21);
	assert(te_out.chests[0].items[0].id == 265
		   && te_out.chests[0].items[0].count == 7);
	assert(te_out.chests[0].items[26].id == 331
		   && te_out.chests[0].items[26].count == 64);
	assert(te_out.chests[1].x == 24 && te_out.chests[1].items[53].id == 351
		   && te_out.chests[1].items[53].durability == 3);
	assert(te_out.chests[1].items[0].id == 278
		   && te_out.chests[1].items[0].durability == 1500); // NOT 1500 % 256 == 220
	assert(te_out.signs[0].x == 17 && te_out.signs[0].y == 65
		   && te_out.signs[0].z == 18);
	assert(strncmp(te_out.signs[0].text, "a saved sign", 12) == 0);
	assert(te_out.n_furnace == 1);
	assert(te_out.furnaces[0].x == 22 && te_out.furnaces[0].y == 64
		   && te_out.furnaces[0].z == 25);
	assert(te_out.furnaces[0].burn_time == 412
		   && te_out.furnaces[0].cook_time == 173); // shorts survive the .mcr path
	assert(te_out.furnaces[0].items[0].id == 15
		   && te_out.furnaces[0].items[0].count == 8);
	assert(te_out.furnaces[0].items[1].id == 263
		   && te_out.furnaces[0].items[1].count == 3);
	assert(te_out.furnaces[0].items[2].id == 265
		   && te_out.furnaces[0].items[2].count == 4);
	assert(te_out.n_piston_moving == 1);
	assert(te_out.piston_movings[0].x == 31
		   && te_out.piston_movings[0].y == 127
		   && te_out.piston_movings[0].z == 16);
	assert(te_out.piston_movings[0].stored_id == 255
		   && te_out.piston_movings[0].stored_meta == 15
		   && te_out.piston_movings[0].facing == 5);
	assert(te_out.piston_movings[0].progress == 0.5F
		   && te_out.piston_movings[0].extending == 1);
	assert(te_out.n_dispenser == 1);
	assert(te_out.dispensers[0].x == 26 && te_out.dispensers[0].y == 68
		   && te_out.dispensers[0].z == 20);
	assert(te_out.dispensers[0].items[0].id == 262
		   && te_out.dispensers[0].items[0].count == 5);
	assert(te_out.dispensers[0].items[8].id == 332
		   && te_out.dispensers[0].items[8].count == 16);
	for(int k = 1; k < 8; k++)
		assert(te_out.dispensers[0].items[k].id == 0);
	assert(te_out.n_note == 1);
	assert(te_out.notes[0].x == 18 && te_out.notes[0].y == 66
		   && te_out.notes[0].z == 24);
	assert(te_out.notes[0].note == 17);
	assert(te_out.n_record_player == 1);
	assert(te_out.record_players[0].x == 29 && te_out.record_players[0].y == 69
		   && te_out.record_players[0].z == 19);
	assert(te_out.record_players[0].record == 2257);

	// Cleanup. sc2's terrain arrays are mem2_alloc'd copies (the MEM1-pinning
	// cNBT borrow was removed) -- release through the range-aware mem2_free,
	// matching server_world_chunk_destroy.
	tile_entities_parse_free(&te_out);
	size_t reload_free0 = mem2_stat_free_calls();
	mem2_free(sc2.ids);
	mem2_free(sc2.metadata);
	mem2_free(sc2.lighting_sky);
	mem2_free(sc2.lighting_torch);
	mem2_free(sc2.heightmap);
	assert(mem2_stat_free_calls() - reload_free0 == 5);

	// Parse OOM is a chunk-read failure, not a partially published TE set. The
	// region layer must also release every terrain allocation it made first.
	struct server_chunk failed_chunk = {0};
	struct chunk_tile_entities failed_te = {0};
	tile_entities_nbt_test_fail_allocation_after(0);
	assert(!region_archive_get_blocks(
		&ra, TEST_CHUNK_X, TEST_CHUNK_Z, &failed_chunk, &failed_te));
	assert(!failed_chunk.ids && !failed_chunk.metadata
		   && !failed_chunk.lighting_sky && !failed_chunk.lighting_torch
		   && !failed_chunk.heightmap);
	assert(!failed_te.chests && failed_te.n_chest == 0);
	assert(!failed_te.signs && failed_te.n_sign == 0);
	assert(!failed_te.furnaces && failed_te.n_furnace == 0);
	assert(!failed_te.piston_movings && failed_te.n_piston_moving == 0);

	region_archive_destroy(&ra);
	free(sc.ids);
	free(sc.metadata);
	free(sc.lighting_sky);
	free(sc.lighting_torch);
	free(sc.heightmap);
	string_clear(level);
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));

	printf("test_region_tile_entities: full .mcr round-trip OK -- terrain "
		   "byte-identical with 2 chests + 1 sign + 1 furnace + 1 dispenser + "
		   "1 note block + 1 jukebox + 1 piston persisted\n");
	test_fixture_dir_cleanup(TMP_WORLD);
	return 0;
}
