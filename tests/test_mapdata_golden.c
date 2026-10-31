/*
	Golden/round-trip test for the Beta 1.7.3 map DATA layer (gap audit #10,
	mapdata-slice, part 1 of 2) [740c583: MapData, MapDataBase, MapStorage,
	SaveHandler, ItemMap.updateMapData/getMapData].

	Named "_golden" per CODESTYLE 4.1, not "_env": this file drives
	map_data.c/map_data_archive.c directly and does a real on-disk NBT
	round trip, but never touches the full server harness (clin_init,
	blocks_init, server_world_create, ...) that "_env" denotes -- it can't:
	ItemMap (part 2) doesn't exist yet. Originally landed misregistered as
	test_mapdata_env; renamed 2026-08-13 post-review (see
	MAPDATA_SLICE_NOTES.md's post-review section).

	Every expected value below is transcribed either directly from
	MAPDATA_DISPATCH.md's hand-computed tables (the session owner's own
	from-decompile arithmetic; CODESTYLE 4.2 -- never re-derived by running
	our own code) or hand-derived from the cited decompile formula in this
	file's own comments (the map_data_scan_pixel_center cases: true-floor,
	then a truncating integer divide, then +64 -- worked by hand and
	cross-checked with a plain calculator, not produced by any CaveWii code).

	Covers: the Nether column-pick hash (Java int32 wrap through a double
	multiply, incl. negative coordinates), the scale/radius math, the stripe
	gate (full 0..31 sweep per row -- exact set, not a window), the center-
	offset floor+divide chain, the cross-dimension update guard, creation
	defaults, and a real on-disk save/reload round trip of every NBT field
	including the full 16384-byte colour array and the scale<0/>4 clamp. Also
	pins deliberate deviations from a literal decompile port:
	  - a non-128x128 width/height, or a wrong-length colors array even on
	    the 128x128 fast path, fails closed (MapData.readFromNBT's own resize
	    branch has an always-true bounds guard that would be a real OOB write
	    in C, and its fast path trusts an unchecked array length -- see
	    map_data_archive.h);
	  - the one-generation durable-write's crash-window "_new" fallback
	    recovers a save that died before its promoting rename -- INCLUDING a
	    non-fatal rename failure, deterministically forced via test-only
	    fault injection (2026-08-13 post-review: the original code silently
	    deleted that survivor on a non-fatal rename failure instead);
	  - the "_new" fallback fires ONLY when the primary is genuinely absent,
	    never merely because it is present-but-corrupt (2026-08-13
	    post-review: the original code let a stale "_new" survivor mask a
	    corrupted, present primary instead of failing closed).
*/

#include <assert.h>
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
#include "cNBT/nbt.h"
#include "network/map_data.h"
#include "network/map_data_archive.h"

static char s_fixture_MD_WORLD[128]; /* was #define MD_WORLD='test_mapdata_world_tmp' (a literal) */
#define MD_WORLD s_fixture_MD_WORLD

// ---- test-only raw NBT writer: plants files this test's OWN production
// code would never write (a mismatched width/height, or a file sitting only
// at the "_new" crash-recovery name) so the read side's handling of them is
// pinned independently of the write side. Mirrors map_data_archive.c's own
// dump-only-tree shape exactly (never pass `root` to nbt_free/nbt_clone --
// every node here is stack/caller memory).
static void write_raw_map_file(const char* path, int8_t dimension,
							   int32_t x_center, int32_t z_center,
							   int8_t scale, int16_t width, int16_t height,
							   const uint8_t* colors, int32_t colors_len) {
	struct nbt_list data_list_sentinel = (struct nbt_list) {.data = NULL};
	INIT_LIST_HEAD(&data_list_sentinel.entry);

	nbt_node data_nodes[] = {
		{.type = TAG_BYTE, .name = "dimension", .payload.tag_byte = dimension},
		{.type = TAG_INT, .name = "xCenter", .payload.tag_int = x_center},
		{.type = TAG_INT, .name = "zCenter", .payload.tag_int = z_center},
		{.type = TAG_BYTE, .name = "scale", .payload.tag_byte = scale},
		{.type = TAG_SHORT, .name = "width", .payload.tag_short = width},
		{.type = TAG_SHORT, .name = "height", .payload.tag_short = height},
		{.type = TAG_BYTE_ARRAY,
		 .name = "colors",
		 .payload.tag_byte_array.data = (unsigned char*)colors,
		 .payload.tag_byte_array.length = colors_len},
	};

	struct nbt_list data_list[sizeof(data_nodes) / sizeof(*data_nodes)];
	for(size_t k = 0; k < sizeof(data_list) / sizeof(*data_list); k++) {
		data_list[k].data = &data_nodes[k];
		list_add_tail(&data_list[k].entry, &data_list_sentinel.entry);
	}

	nbt_node data_compound = {
		.type = TAG_COMPOUND,
		.name = "data",
		.payload.tag_compound = &data_list_sentinel,
	};
	struct nbt_list root_list_sentinel = {.data = NULL};
	INIT_LIST_HEAD(&root_list_sentinel.entry);
	struct nbt_list root_list = {.data = &data_compound};
	list_add_head(&root_list.entry, &root_list_sentinel.entry);

	nbt_node root = {
		.type = TAG_COMPOUND,
		.name = "",
		.payload.tag_compound = &root_list_sentinel,
	};

	FILE* f = fopen(path, "wb");
	assert(f);
	assert(nbt_dump_file(&root, f, STRAT_GZIP) == NBT_OK);
	assert(fclose(f) == 0);
}

static void check_stripe_row(int32_t step, int32_t col_a, int32_t col_b) {
	// Full 0..31 sweep: exactly col_a and col_b select, every other column
	// in the range is a negative control. MAPDATA_DISPATCH.md's table.
	for(int32_t x = 0; x < 32; x++) {
		bool expected = (x == col_a || x == col_b);
		assert(map_data_scan_stripe_selected(x, step) == expected);
	}
}

static bool file_exists(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	fclose(f);
	return true;
}

int main(void) {
	test_fixture_dir("test_mapdata_world_tmp", s_fixture_MD_WORLD, sizeof s_fixture_MD_WORLD);
	// ---- clean slate: remove anything a previous (possibly crashed) run
	// left behind for every id this test touches.
	MKDIR(MD_WORLD);
	MKDIR(test_fixture_join(MD_WORLD, "/data"));
	for(int id = 0; id <= 7; id++) {
		char path[256];
		snprintf(path, sizeof(path), test_fixture_join(MD_WORLD, "/data/map_%d.dat"), id);
		remove(path);
		snprintf(path, sizeof(path), test_fixture_join(MD_WORLD, "/data/map_%d.dat_new"), id);
		remove(path);
	}

	// ==== MAPDATA_DISPATCH.md table 1: the Nether column pick ================
	// [740c583: ItemMap.updateMapData, hasNoSky branch] -- transcribed
	// verbatim from the dispatch, NOT recomputed.
	{
		struct {
			int32_t x, z;
			bool stone;
		} rows[] = {
			{0, 0, false},      {1, 0, true},         {0, 1, true},
			{16, 16, true},     {-1, -1, true},       {100, -200, false},
			{-1000, 1000, true}, {7, 13, true},
		};
		for(size_t i = 0; i < sizeof(rows) / sizeof(*rows); i++)
			assert(map_data_scan_nether_column_is_stone(rows[i].x, rows[i].z)
				  == rows[i].stone);
	}

	// ==== MAPDATA_DISPATCH.md table 2: scale math =============================
	{
		struct {
			int8_t scale;
			int32_t blocks_per_pixel, radius_overworld, radius_nether;
		} rows[] = {
			{0, 1, 128, 64}, {1, 2, 64, 32}, {2, 4, 32, 16},
			{3, 8, 16, 8},   {4, 16, 8, 4},
		};
		for(size_t i = 0; i < sizeof(rows) / sizeof(*rows); i++) {
			int32_t bpp = map_data_scan_blocks_per_pixel(rows[i].scale);
			assert(bpp == rows[i].blocks_per_pixel);
			assert(map_data_scan_radius(bpp, false) == rows[i].radius_overworld);
			assert(map_data_scan_radius(bpp, true) == rows[i].radius_nether);
		}
	}

	// ==== MAPDATA_DISPATCH.md table 3: the stripe gate =========================
	check_stripe_row(1, 1, 17);
	check_stripe_row(2, 2, 18);
	check_stripe_row(16, 0, 16);
	check_stripe_row(17, 1, 17);

	// ==== center offsets: hand-derived from the decompile formula ============
	// [740c583: ItemMap.updateMapData] floors the player-minus-centre
	// offset, divides by blocks-per-pixel, adds 64. Each expected value below is
	// floor(pos-center), truncating-divided by bpp, plus 64 -- worked by
	// hand (see MAPDATA_SLICE_NOTES.md for the worked arithmetic), not read
	// back from this file's own map_data_scan_pixel_center. Case D is the
	// load-bearing one: a TRUE floor of -0.5 is -1, not 0 -- the truncating
	// cast this project's own CODESTYLE 2.3 warns about would silently give
	// 64 instead of the correct 63.
	{
		struct {
			double pos;
			int32_t center, bpp, expected;
		} rows[] = {
			{0.0, 0, 8, 64},     // A: at center, scale 3
			{100.0, 0, 8, 76},   // B: floor(100)/8=12, +64=76
			{-100.0, 0, 8, 52},  // C: floor(-100)/8=-12, +64=52
			{-0.5, 0, 1, 63},    // D: floor(-0.5)=-1 (TRUE floor, not trunc)
			{7.999, 0, 4, 65},   // E: floor(7.999)=7, 7/4=1, +64=65
			{250.0, 50, 2, 164}, // F: floor(200)/2=100, +64=164
			{10.0, 100, 16, 59}, // G: floor(-90)/16=-5 (trunc toward 0), +64=59
			{10.3, 20, 8, 63},   // H: floor(-9.7)=-10, -10/8=-1 (trunc), +64=63
		};
		for(size_t i = 0; i < sizeof(rows) / sizeof(*rows); i++)
			assert(map_data_scan_pixel_center(rows[i].pos, rows[i].center,
											  rows[i].bpp)
				  == rows[i].expected);
	}

	// ==== the cross-dimension update guard ====================================
	// [740c583: ItemMap.updateMapData] `if (worldProvider.worldType ==
	// mapData.dimension)`. Positive AND negative controls both ways.
	assert(map_data_scan_should_update(WORLD_DIM_OVERWORLD, 0) == true);
	assert(map_data_scan_should_update(WORLD_DIM_NETHER, -1) == true);
	assert(map_data_scan_should_update(WORLD_DIM_OVERWORLD, -1) == false);
	assert(map_data_scan_should_update(WORLD_DIM_NETHER, 0) == false);

	// ==== creation defaults [740c583: ItemMap.getMapData] =====================
	{
		struct map_data md;
		memset(&md, 0xAA, sizeof(md)); // poison: every field must be SET, not left over
		map_data_set_defaults(&md, /*x_center=*/100, /*z_center=*/-50,
							  WORLD_DIM_OVERWORLD);
		assert(md.x_center == 100);
		assert(md.z_center == -50);
		assert(md.dimension == WORLD_DIM_OVERWORLD);
		assert(md.scale == 3); // "a fresh map is created at scale 3"
		assert(md.step == 0);
		for(int i = 0; i < MAP_DATA_COLORS_SIZE; i++)
			assert(md.colors[i] == 0); // Java `new byte[16384]` zero-fills
		// scale 3 => 8 blocks/pixel, radius 16 (dispatch's scale-math table).
		assert(map_data_scan_blocks_per_pixel(md.scale) == 8);
		assert(map_data_scan_radius(8, false) == 16);

		// Second call site: a different (dimension, center) pair must land
		// exactly as given -- proves the function does not hardcode either.
		memset(&md, 0x55, sizeof(md));
		map_data_set_defaults(&md, /*x_center=*/-8, /*z_center=*/16,
							  WORLD_DIM_NETHER);
		assert(md.x_center == -8);
		assert(md.z_center == 16);
		assert(md.dimension == WORLD_DIM_NETHER);
		assert(md.scale == 3);
	}

	// ==== real on-disk round trip: every NBT field, all 16384 colour bytes ===
	{
		string_t world_dir;
		string_init_set_str(world_dir, MD_WORLD);

		struct map_data md;
		md.x_center = 12345;
		md.z_center = -6789;
		md.dimension = WORLD_DIM_NETHER; // exercises a negative dimension byte
		md.scale = 2;                    // in-range: read-back must be exact
		md.step = 999;                   // must NOT survive (never persisted)
		for(int i = 0; i < MAP_DATA_COLORS_SIZE; i++)
			md.colors[i] = (uint8_t)(i * 7 + 3); // arbitrary fill (test_region_boundary.c's id_pattern idiom)

		assert(map_data_archive_save(world_dir, 0, &md));

		struct map_data loaded;
		memset(&loaded, 0xAA, sizeof(loaded)); // poison before load
		assert(map_data_archive_load(world_dir, 0, &loaded));

		assert(loaded.x_center == 12345);
		assert(loaded.z_center == -6789);
		assert(loaded.dimension == WORLD_DIM_NETHER);
		assert(loaded.scale == 2);
		assert(loaded.step == 0); // readFromNBT never assigns step
		for(int i = 0; i < MAP_DATA_COLORS_SIZE; i++)
			assert(loaded.colors[i] == (uint8_t)(i * 7 + 3));

		string_clear(world_dir);
	}

	// ==== the scale<0 / scale>4 clamp [740c583: MapData.readFromNBT] =========
	{
		string_t world_dir;
		string_init_set_str(world_dir, MD_WORLD);

		struct map_data md;
		map_data_set_defaults(&md, 0, 0, WORLD_DIM_OVERWORLD);

		md.scale = -5; // writeToNBT itself does NOT clamp (raw byte write) --
		assert(map_data_archive_save(world_dir, 1, &md)); // only readFromNBT does.
		struct map_data loaded;
		assert(map_data_archive_load(world_dir, 1, &loaded));
		assert(loaded.scale == 0);

		md.scale = 9;
		assert(map_data_archive_save(world_dir, 2, &md));
		assert(map_data_archive_load(world_dir, 2, &loaded));
		assert(loaded.scale == 4);

		string_clear(world_dir);
	}

	// ==== absent file: load fails, load_or_create falls back to defaults ====
	{
		string_t world_dir;
		string_init_set_str(world_dir, MD_WORLD);

		struct map_data loaded;
		memset(&loaded, 0xAA, sizeof(loaded));
		assert(!map_data_archive_load(world_dir, 3, &loaded)); // never saved

		struct map_data created;
		memset(&created, 0xAA, sizeof(created));
		map_data_load_or_create(world_dir, 3, /*spawn_x=*/64, /*spawn_z=*/-32,
								WORLD_DIM_OVERWORLD, &created);
		assert(created.x_center == 64);
		assert(created.z_center == -32);
		assert(created.dimension == WORLD_DIM_OVERWORLD);
		assert(created.scale == 3);
		assert(created.step == 0);
		for(int i = 0; i < MAP_DATA_COLORS_SIZE; i++)
			assert(created.colors[i] == 0);

		string_clear(world_dir);
	}

	// ==== deviation: non-128x128 fails closed instead of Beta's always-true- ==
	// guarded (and, in C, genuinely unsafe) resize branch.
	// [740c583: MapData.readFromNBT] -- see map_data_archive.h.
	{
		uint8_t small_colors[64 * 64];
		memset(small_colors, 0x11, sizeof(small_colors));
		write_raw_map_file(test_fixture_join(MD_WORLD, "/data/map_4.dat"), WORLD_DIM_OVERWORLD, 0,
						   0, 3, 64, 64, small_colors, sizeof(small_colors));

		string_t world_dir;
		string_init_set_str(world_dir, MD_WORLD);
		struct map_data loaded;
		memset(&loaded, 0xAA, sizeof(loaded));
		assert(!map_data_archive_load(world_dir, 4, &loaded));
		string_clear(world_dir);
	}

	// ==== deviation: the durable-write crash window is covered by the ======
	// "_new" fallback -- a save that completed its write+fsync but died
	// before the promoting rename leaves ONLY "<id>.dat_new" on disk. A
	// successful recovery also self-heals by promoting "_new" onto the
	// primary name (2026-08-13 post-review finding #3, second half) --
	// proven here by checking the FILESYSTEM directly, not just re-loading
	// (a non-promoting implementation would also re-load successfully via
	// the same fallback, so only a direct file-existence check discriminates
	// "promoted" from "left at _new forever").
	{
		uint8_t colors[MAP_DATA_COLORS_SIZE];
		for(int i = 0; i < MAP_DATA_COLORS_SIZE; i++)
			colors[i] = (uint8_t)(i * 3 + 1);
		// dimension = WORLD_DIM_NETHER (-1), not 0: table 5's raw-NBT writer
		// previously only ever wrote dimension=0 here, so the read side's
		// "dimension" NBT-key handling was never independently checked for a
		// negative byte anywhere but the self-referential save/load round
		// trip above (2026-08-13 post-review finding #5) -- a shared
		// save+load bug (wrong key, or a dropped sign) could have passed
		// that round trip while still loading a hand-written/real-Beta file
		// wrong. -1 forces sign-extension to actually matter here.
		write_raw_map_file(test_fixture_join(MD_WORLD, "/data/map_5.dat_new"), WORLD_DIM_NETHER,
						   555, -777, 1, MAP_DATA_DIM, MAP_DATA_DIM, colors,
						   MAP_DATA_COLORS_SIZE);
		// The committed name must be absent -- only the survivor exists.
		remove(test_fixture_join(MD_WORLD, "/data/map_5.dat"));

		string_t world_dir;
		string_init_set_str(world_dir, MD_WORLD);
		struct map_data loaded;
		memset(&loaded, 0xAA, sizeof(loaded));
		assert(map_data_archive_load(world_dir, 5, &loaded));
		assert(loaded.x_center == 555);
		assert(loaded.z_center == -777);
		assert(loaded.dimension == WORLD_DIM_NETHER); // finding #5: independently checked
		assert(loaded.scale == 1);
		for(int i = 0; i < MAP_DATA_COLORS_SIZE; i++)
			assert(loaded.colors[i] == (uint8_t)(i * 3 + 1));
		// finding #3 (second half): the survivor must have been PROMOTED --
		// the primary name exists on disk now, not just recoverable via
		// "_new" forever.
		assert(file_exists(test_fixture_join(MD_WORLD, "/data/map_5.dat")));
		string_clear(world_dir);
	}

	// ==== post-review findings #1+#2: a promoting-rename FAILURE (not a =====
	// crash) must not delete the just-written, fsynced "_new" survivor.
	// Forced deterministically via test-only fault injection (mirrors
	// death_drop_journal.c's CAVEWII_DEATH_DROP_TEST / region_archive.c's
	// CAVEWII_REGION_ARCHIVE_TEST shape) instead of an OS-specific race.
	// Reproduces the exact scenario both reviewers flagged: save#1 commits
	// OLD data; save#2's write+fsync succeeds and remove(path) runs, but the
	// promoting rename() then fails -- the original code deleted the just-
	// written survivor too, losing BOTH copies and silently resetting the
	// map to blank on the next load.
	{
		string_t world_dir;
		string_init_set_str(world_dir, MD_WORLD);

		struct map_data original;
		map_data_set_defaults(&original, /*x_center=*/1, /*z_center=*/1,
							  WORLD_DIM_OVERWORLD);
		assert(map_data_archive_save(world_dir, 6, &original)); // "save#1": OLD committed

		struct map_data updated;
		map_data_set_defaults(&updated, /*x_center=*/2222, /*z_center=*/-3333,
							  WORLD_DIM_OVERWORLD);
		for(int i = 0; i < MAP_DATA_COLORS_SIZE; i++)
			updated.colors[i] = (uint8_t)(i + 9);

		map_data_archive_test_fail_next(MAP_DATA_ARCHIVE_TEST_FAIL_RENAME);
		// "save#2": write+fsync succeed, remove(path) runs, injected rename
		// failure -- must report failure...
		assert(!map_data_archive_save(world_dir, 6, &updated));

		// ...but must NOT have destroyed the fsynced survivor: a load must
		// recover the UPDATED (not the OLD, and not blank/failed) data via
		// the "_new" fallback. Under the pre-fix code this assert fails --
		// map_data_archive_load returns false because BOTH path and
		// path_new were gone.
		struct map_data recovered;
		memset(&recovered, 0xAA, sizeof(recovered));
		assert(map_data_archive_load(world_dir, 6, &recovered));
		assert(recovered.x_center == 2222);
		assert(recovered.z_center == -3333);
		for(int i = 0; i < MAP_DATA_COLORS_SIZE; i++)
			assert(recovered.colors[i] == (uint8_t)(i + 9));

		string_clear(world_dir);
	}

	// ==== post-review finding #3 (first half): a PRESENT-but-corrupt =========
	// primary must fail closed, never silently substitute a stale (valid,
	// but superseded) "_new" survivor. The fallback exists ONLY for
	// "nothing at path" (an interrupted promoting rename) -- not for "path
	// exists but its bytes are bad" (e.g. real on-disk bit-rot, unrelated to
	// any save crash). Reproduces finding #3's own scenario: a stale valid
	// _new plus a corrupted (here: not-even-NBT) present primary.
	{
		uint8_t stale_colors[MAP_DATA_COLORS_SIZE];
		memset(stale_colors, 0x42, sizeof(stale_colors));
		write_raw_map_file(test_fixture_join(MD_WORLD, "/data/map_7.dat_new"), WORLD_DIM_OVERWORLD,
						   999, 999, 0, MAP_DATA_DIM, MAP_DATA_DIM,
						   stale_colors, MAP_DATA_COLORS_SIZE);

		// The PRESENT primary: not valid NBT/gzip at all (simulated
		// bit-rot) -- distinct from the width/height deviation test above,
		// which is well-formed NBT with a rejected shape. Both flavors of
		// "present but unusable" must equally skip the fallback.
		FILE* garbage = fopen(test_fixture_join(MD_WORLD, "/data/map_7.dat"), "wb");
		assert(garbage);
		const char junk[] = "not an nbt file, just bit-rotted bytes";
		assert(fwrite(junk, sizeof(junk), 1, garbage) == 1);
		assert(fclose(garbage) == 0);

		string_t world_dir;
		string_init_set_str(world_dir, MD_WORLD);
		struct map_data loaded;
		memset(&loaded, 0xAA, sizeof(loaded));
		// Must fail closed -- NOT silently return the stale _new's
		// (999, 999). Under the pre-fix code this assert fails: the
		// fallback fires on ANY primary-parse failure, so it would return
		// true with x_center==999.
		assert(!map_data_archive_load(world_dir, 7, &loaded));
		string_clear(world_dir);
	}

	printf("test_mapdata_golden: OK (nether hash, scale/stripe/center math, "
		  "dimension guard incl. sign, defaults, on-disk round trip, clamp, "
		  "fail-closed resize, rename-failure survivor recovery, "
		  "present-corrupt-fails-closed, _new self-heal promotion)\n");
	test_fixture_dir_cleanup(MD_WORLD);
	return 0;
}
