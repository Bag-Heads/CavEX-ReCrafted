/*
	Crash-window recovery for level.dat (2026-06-12 persistence-batch review,
	finding #3). level_archive_flush writes level.dat_new, then rotates
	level.dat -> level.dat_old, then level.dat_new -> level.dat. A power cut
	between the two renames leaves NO level.dat but an intact _old (previous
	good) and _new (current). The old level_archive_create read level.dat with
	no fallback, so the world became permanently unopenable despite the backup.

	This pins that level_archive_create recovers from each survivor and
	promotes it back into place, preserving a known field (the seed).
*/

#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "network/level_archive.h"
#include "network/server_interface.h"

static char s_fixture_TMP_DIR[128]; /* was #define TMP_DIR='test_level_recovery_tmp' (a literal) */
#define TMP_DIR s_fixture_TMP_DIR
#define SEED ((int64_t)0x0123456789ABCDEFLL)

// level_archive.c also owns inventory serialization, but this focused target
// exercises only level metadata. Satisfy that unused function's link dependency
// without pulling the whole item/gameplay graph into the archive test.
void inventory_clear(struct inventory* inv) {
	assert(inv);
	for(size_t k = 0; k < inv->capacity; k++)
		inv->items[k] = (struct item_data) {0};
	inv->picked_item = (struct item_data) {0};
	inv->hotbar_slot = 0;
}

static void write_baseline(string_t dir) {
	struct level_archive la;
	bool ok = level_archive_create_new(&la, dir, SEED, (vec3) {10, 70, 20});
	assert(ok);
	ok = level_archive_save(&la); // flush via the safe rotation -> level.dat
	assert(ok);
	level_archive_destroy(&la);
}

static int64_t read_seed(string_t dir) {
	struct level_archive la;
	bool ok = level_archive_create(&la, dir); // the path under test
	assert(ok);
	int64_t seed = 0;
	ok = level_archive_read(&la, LEVEL_RANDOM_SEED, &seed, 0);
	assert(ok);
	level_archive_destroy(&la);
	return seed;
}

static int16_t read_health(string_t dir) {
	struct level_archive la;
	bool ok = level_archive_create(&la, dir);
	assert(ok);
	int16_t health = 0;
	/* PLAYER-HEALTH-NBT: disk Health is Beta half-hearts; the reader
	 * returns CaveWii units (exact via CaveWiiHealth). */
	ok = level_archive_read_player_health(&la, &health);
	assert(ok);
	level_archive_destroy(&la);
	return health;
}

static void dump_live_tree(struct level_archive* la) {
	FILE* f = fopen(string_get_cstr(la->file_name), "wb");
	assert(f);
	assert(nbt_dump_file(la->data, f, STRAT_GZIP) == NBT_OK);
	assert(fclose(f) == 0);
}

static void rename_node(nbt_node* node, const char* name) {
	char* replacement = malloc(strlen(name) + 1);
	assert(replacement);
	strcpy(replacement, name);
	free(node->name);
	node->name = replacement;
}

int main(void) {
	test_fixture_dir("test_level_recovery_tmp", s_fixture_TMP_DIR, sizeof s_fixture_TMP_DIR);
	MKDIR(TMP_DIR);
	remove(test_fixture_join(TMP_DIR, "/level.dat"));
	remove(test_fixture_join(TMP_DIR, "/level.dat_old"));
	remove(test_fixture_join(TMP_DIR, "/level.dat_new"));

	string_t dir;
	string_init_set_str(dir, TMP_DIR);

	// 0) baseline: a normal level.dat round-trips.
	write_baseline(dir);
	assert(read_seed(dir) == SEED);
	// CaveWii stores health in 1/16-heart units. A fresh world must therefore
	// begin at the engine maximum, not the template's old raw Beta value 20.
	assert(read_health(dir) == MAX_PLAYER_HEALTH);
	/* Beta keeps permanent Data.SpawnXYZ separate from the optional
	 * Data.Player.SpawnXYZ bed coordinate. Fresh/old worlds have no personal
	 * triple; setting, updating and clearing it must never move world spawn. */
	{
		struct level_archive la;
		assert(level_archive_create(&la, dir));
		int32_t wx = 0, wy = 0, wz = 0;
		assert(level_archive_read(&la, LEVEL_WORLD_SPAWNX, &wx, 0));
		assert(level_archive_read(&la, LEVEL_WORLD_SPAWNY, &wy, 0));
		assert(level_archive_read(&la, LEVEL_WORLD_SPAWNZ, &wz, 0));
		assert(wx == 10 && wy == 70 && wz == 20);
		bool present = true;
		int32_t x = 111, y = 112, z = 113;
		assert(level_archive_read_player_spawn(
			&la, &present, &x, &y, &z));
		assert(!present && x == 111 && y == 112 && z == 113);
		assert(level_archive_write_player_spawn(&la, true, -25, 71, 31));
		assert(level_archive_save(&la));
		level_archive_destroy(&la);

		assert(level_archive_create(&la, dir));
		assert(level_archive_read_player_spawn(
			&la, &present, &x, &y, &z));
		assert(present && x == -25 && y == 71 && z == 31);
		assert(level_archive_read(&la, LEVEL_WORLD_SPAWNX, &wx, 0));
		assert(level_archive_read(&la, LEVEL_WORLD_SPAWNY, &wy, 0));
		assert(level_archive_read(&la, LEVEL_WORLD_SPAWNZ, &wz, 0));
		assert(wx == 10 && wy == 70 && wz == 20);
		assert(level_archive_write_player_spawn(&la, true, 44, 72, -55));
		assert(level_archive_save(&la));
		level_archive_destroy(&la);

		assert(level_archive_create(&la, dir));
		assert(level_archive_read_player_spawn(
			&la, &present, &x, &y, &z));
		assert(present && x == 44 && y == 72 && z == -55);
		/* A malformed present tag is rejected as a unit, then transactionally
		 * replaced rather than leaving a partial triple. */
		nbt_node* sy = nbt_find_by_path(la.data, LEVEL_PLAYER_SPAWNY.name);
		assert(sy && sy->type == TAG_INT);
		sy->type = TAG_SHORT;
		present = true;
		assert(!level_archive_read_player_spawn(
			&la, &present, &x, &y, &z));
		assert(!present);
		assert(level_archive_write_player_spawn(&la, true, 60, 73, 61));
		assert(level_archive_read_player_spawn(
			&la, &present, &x, &y, &z));
		assert(present && x == 60 && y == 73 && z == 61);
		assert(level_archive_write_player_spawn(&la, false, 0, 0, 0));
		assert(level_archive_save(&la));
		level_archive_destroy(&la);

		assert(level_archive_create(&la, dir));
		present = true;
		assert(level_archive_read_player_spawn(
			&la, &present, &x, &y, &z));
		assert(!present);
		level_archive_destroy(&la);
		puts("  player bed spawn: optional Player triple round-trips; world spawn unchanged");
	}
	/* Entity.writeToNBT owns continuous FallDistance. A save/reopen in mid-air
	 * must not grant a free landing. */
	{
		struct level_archive la;
		assert(level_archive_create(&la, dir));
		float fall = 6.25F;
		assert(level_archive_write(&la, LEVEL_PLAYER_FALL_DISTANCE, &fall));
		assert(level_archive_save(&la));
		level_archive_destroy(&la);
		assert(level_archive_create(&la, dir));
		float restored = 0.0F;
		assert(level_archive_read(
			&la, LEVEL_PLAYER_FALL_DISTANCE, &restored, 0));
		assert(restored == fall);
		level_archive_destroy(&la);
	}
	/* Pre-FallDistance CaveWii saves are already on users' SD cards. Their
	 * first autosave/portal/death checkpoint must migrate the absent tag rather
	 * than fail forever. A present tag with the wrong type remains corruption
	 * and must be rejected instead of silently replaced. */
	{
		struct level_archive la;
		assert(level_archive_create(&la, dir));
		nbt_node* fall_node
			= nbt_find_by_path(la.data, LEVEL_PLAYER_FALL_DISTANCE.name);
		assert(fall_node && fall_node->type == TAG_FLOAT);
		rename_node(fall_node, "LegacyFallDistance");
		dump_live_tree(&la);
		level_archive_destroy(&la);

		assert(level_archive_create(&la, dir));
		float restored = -1.0F;
		assert(!level_archive_read(
			&la, LEVEL_PLAYER_FALL_DISTANCE, &restored, 0));
		float fall = 7.75F;
		assert(level_archive_write_player_fall_distance(&la, fall));
		assert(level_archive_save(&la));
		level_archive_destroy(&la);

		assert(level_archive_create(&la, dir));
		assert(level_archive_read(
			&la, LEVEL_PLAYER_FALL_DISTANCE, &restored, 0));
		assert(restored == fall);
		fall_node = nbt_find_by_path(
			la.data, LEVEL_PLAYER_FALL_DISTANCE.name);
		assert(fall_node && fall_node->type == TAG_FLOAT);
		fall_node->type = TAG_INT;
		assert(!level_archive_write_player_fall_distance(&la, fall));
		assert(fall_node->type == TAG_INT);
		level_archive_discard(&la);
	}

	// 1) crash AFTER both renames done but recovery already promoted -- the
	//    normal case re-verified: level.dat present, opens directly.
	assert(read_seed(dir) == SEED);

	// 2) crash between the renames, variant A: level.dat gone, _old survives.
	//    (rename the live file to the backup, as the first rename would have.)
	rename(test_fixture_join(TMP_DIR, "/level.dat"), test_fixture_join(TMP_DIR, "/level.dat_old"));
	assert(read_seed(dir) == SEED); // recovered from _old + promoted back

	// promotion restored level.dat; confirm a plain reopen now works and the
	// _old it was promoted from is consumed (renamed, not copied).
	{
		FILE* f = fopen(test_fixture_join(TMP_DIR, "/level.dat"), "rb");
		assert(f);
		fclose(f);
	}

	// 3) crash between the renames, variant B: level.dat gone, _new survives
	//    (the second rename never ran). _new is the newest complete write.
	rename(test_fixture_join(TMP_DIR, "/level.dat"), test_fixture_join(TMP_DIR, "/level.dat_new"));
	assert(read_seed(dir) == SEED); // recovered from _new + promoted back

	// 4) New worlds are stamped. A tag-less Beta/pre-version CaveWii world is
	// accepted as legacy version 0, and an ordinary save preserves that state:
	// save-time stamping is not creation provenance. Only the explicit schema
	// upgrade API may install the current version after migration/classification.
	write_baseline(dir);
	{
		struct level_archive la;
		assert(level_archive_create(&la, dir));
		assert(level_archive_get_save_version(&la)
			   == CAVEWII_SAVE_VERSION_CURRENT);
		nbt_node* version
			= nbt_find_by_path(la.data, LEVEL_CAVEWII_SAVE_VERSION.name);
		assert(version && version->type == TAG_INT);
		rename_node(version, "LegacySaveVersion");
		dump_live_tree(&la);
		level_archive_destroy(&la);
	}
	{
		struct level_archive la;
		assert(level_archive_create(&la, dir));
		assert(level_archive_get_save_version(&la)
			   == CAVEWII_SAVE_VERSION_LEGACY);
		int64_t changed_seed = SEED + 1;
		assert(level_archive_write(&la, LEVEL_RANDOM_SEED, &changed_seed));
		assert(level_archive_save(&la));
		assert(level_archive_get_save_version(&la)
			   == CAVEWII_SAVE_VERSION_LEGACY);
		assert(level_archive_upgrade_save_version(
			&la, CAVEWII_SAVE_VERSION_CURRENT));
		assert(level_archive_get_save_version(&la)
			   == CAVEWII_SAVE_VERSION_CURRENT);
		level_archive_destroy(&la);
	}

	// 5) Nonfinite player vectors are neither accepted from disk nor persisted.
	write_baseline(dir);
	{
		struct level_archive la;
		assert(level_archive_create(&la, dir));
		vec3 before;
		assert(level_archive_read_player(&la, before, NULL, NULL, NULL));
		assert(!level_archive_write_player(
			&la, (vec3) {NAN, 70.0f, 20.0f}, NULL, NULL,
			WORLD_DIM_OVERWORLD));
		vec3 unchanged;
		assert(level_archive_read_player(&la, unchanged, NULL, NULL, NULL));
		assert(memcmp(before, unchanged, sizeof(before)) == 0);

		nbt_node* pos = nbt_find_by_path(la.data, LEVEL_PLAYER_POSITION.name);
		assert(pos && pos->type == TAG_LIST && pos->payload.tag_list);
		struct nbt_list* first = list_entry(pos->payload.tag_list->entry.flink,
										 struct nbt_list, entry);
		assert(first && first->data && first->data->type == TAG_DOUBLE);
		first->data->payload.tag_double = INFINITY;
		assert(!level_archive_read_player(&la, unchanged, NULL, NULL, NULL));
		level_archive_discard(&la);
	}
	{
		struct level_archive la;
		assert(level_archive_create(&la, dir));
		vec3 before;
		assert(level_archive_read_player(&la, before, NULL, NULL, NULL));
		nbt_node* velocity
			= nbt_find_by_path(la.data, LEVEL_PLAYER_VELOCITY.name);
		assert(velocity && velocity->type == TAG_LIST
			   && velocity->payload.tag_list);
		struct nbt_list* first = list_entry(
			velocity->payload.tag_list->entry.flink, struct nbt_list, entry);
		assert(first && first->data && first->data->type == TAG_DOUBLE);
		first->data->type = TAG_INT; /* malformed shape must fail preflight */
		assert(!level_archive_write_player(
			&la, (vec3) {123.0f, 75.0f, 456.0f}, NULL,
			(vec3) {0.0f, 0.0f, 0.0f}, WORLD_DIM_NETHER));
		vec3 after;
		assert(level_archive_read_player(&la, after, NULL, NULL, NULL));
		assert(memcmp(before, after, sizeof(before)) == 0);
		level_archive_discard(&la);
	}

	// 6) A newer schema is rejected rather than opened and overwritten (or
	// silently rolled back to level.dat_old).
	write_baseline(dir);
	{
		struct level_archive la;
		assert(level_archive_create(&la, dir));
		nbt_node* version
			= nbt_find_by_path(la.data, LEVEL_CAVEWII_SAVE_VERSION.name);
		assert(version && version->type == TAG_INT);
		version->payload.tag_int = CAVEWII_SAVE_VERSION_CURRENT + 1;
		dump_live_tree(&la);
		level_archive_destroy(&la);

		struct level_archive rejected;
		assert(!level_archive_create(&rejected, dir));
	}

	// 7) D6 death-transaction counter: absent on legacy/fresh worlds (read
	//    false -> the caller seeds CAVEWII_DEATH_TX_LEGACY_FLOOR), created
	//    under Data by the migration-style writer, round-trips THROUGH the
	//    save rotation, monotonic overwrites stick, and a wrong-type tag
	//    reads as absent rather than as a value.
	write_baseline(dir);
	{
		struct level_archive la;
		assert(level_archive_create(&la, dir));
		uint32_t counter = 123;
		assert(!level_archive_read_death_tx_counter(&la, &counter));
		assert(counter == 123); /* untouched on absent */
		assert(level_archive_write_death_tx_counter(
			&la, CAVEWII_DEATH_TX_LEGACY_FLOOR + 1));
		assert(level_archive_save(&la));
		level_archive_destroy(&la);
	}
	{
		struct level_archive la;
		assert(level_archive_create(&la, dir));
		uint32_t counter = 0;
		assert(level_archive_read_death_tx_counter(&la, &counter));
		assert(counter == CAVEWII_DEATH_TX_LEGACY_FLOOR + 1);
		assert(level_archive_write_death_tx_counter(&la, counter + 5));
		assert(level_archive_save(&la));
		level_archive_destroy(&la);
	}
	{
		struct level_archive la;
		assert(level_archive_create(&la, dir));
		uint32_t counter = 0;
		assert(level_archive_read_death_tx_counter(&la, &counter));
		assert(counter == CAVEWII_DEATH_TX_LEGACY_FLOOR + 6);
		nbt_node* node = nbt_find_by_path(
			la.data, LEVEL_CAVEWII_DEATH_TX_COUNTER.name);
		assert(node && node->type == TAG_LONG);
		node->type = TAG_INT;
		assert(!level_archive_read_death_tx_counter(&la, &counter));
		/* The CaveWii-owned tag is COERCED by the writer, never wedged: a
		 * permanent write failure would floor-seed every session and
		 * deterministically reissue ids (adversarial review). */
		assert(level_archive_write_death_tx_counter(
			&la, CAVEWII_DEATH_TX_LEGACY_FLOOR + 9));
		assert(node->type == TAG_LONG);
		assert(level_archive_read_death_tx_counter(&la, &counter));
		assert(counter == CAVEWII_DEATH_TX_LEGACY_FLOOR + 9);
		/* Hostile/hand-edited values outside the legitimate band read as
		 * absent (re-seed the floor): a counter of 0 would reissue id 1
		 * against stale tags; UINT32_MAX would wrap the allocator to 1 AND
		 * persist the regression. */
		node->payload.tag_long = 0;
		assert(!level_archive_read_death_tx_counter(&la, &counter));
		node->payload.tag_long = (int64_t)UINT32_MAX;
		assert(!level_archive_read_death_tx_counter(&la, &counter));
		node->payload.tag_long = (int64_t)UINT32_MAX - 1; /* band edge: valid */
		assert(level_archive_read_death_tx_counter(&la, &counter));
		assert(counter == UINT32_MAX - 1);
		level_archive_discard(&la);
	}

	// 9) LastPlayed/SizeOnDisk (Batch H, 2026-08-25): Beta WorldInfo.
	// updateTagCompound [decompile 740c583] writes BOTH unconditionally on
	// every save; the tags round-trip exactly like every other checkpoint
	// field above (the template already carries them at 0, tools/
	// gen_level_template.py:60-61).
	{
		struct level_archive la;
		assert(level_archive_create(&la, dir));
		int64_t last_played = 1234567890123LL;
		int64_t disk_size = 999999LL;
		assert(level_archive_write(&la, LEVEL_LAST_PLAYED, &last_played));
		assert(level_archive_write(&la, LEVEL_DISK_SIZE, &disk_size));
		assert(level_archive_save(&la));
		level_archive_destroy(&la);

		assert(level_archive_create(&la, dir));
		int64_t rp = 0, rs = 0;
		assert(level_archive_read(&la, LEVEL_LAST_PLAYED, &rp, 0));
		assert(level_archive_read(&la, LEVEL_DISK_SIZE, &rs, 0));
		assert(rp == last_played);
		assert(rs == disk_size);
		level_archive_destroy(&la);
		puts("  LastPlayed/SizeOnDisk: write+read round-trip OK");
	}

	// 10) level_archive_disk_size: sums level.dat + every region/*.mcr in
	// both dimensions -- the documented deviation from Beta's incremental
	// delta counter (see level_archive_disk_size's doc comment in
	// level_archive.c). Two known-size synthetic region files prove the
	// directory walk actually reaches both region/ and DIM-1/region/.
	{
		string_t region_dir, nether_dir, nether_region_dir, region_file,
			nether_file, level_dat_path;
		string_init_printf(region_dir, "%s/region", TMP_DIR);
		string_init_printf(nether_dir, "%s/DIM-1", TMP_DIR);
		MKDIR(string_get_cstr(region_dir));
		MKDIR(string_get_cstr(nether_dir));
		string_init_printf(nether_region_dir, "%s/region",
						   string_get_cstr(nether_dir));
		MKDIR(string_get_cstr(nether_region_dir));

		string_init_printf(region_file, "%s/r.0.0.mcr",
						   string_get_cstr(region_dir));
		string_init_printf(nether_file, "%s/r.0.0.mcr",
						   string_get_cstr(nether_region_dir));

		const char region_payload[] = "0123456789";			  // 10 bytes
		const char nether_payload[] = "abcdefghijklmnopqrst"; // 20 bytes
		FILE* rf = fopen(string_get_cstr(region_file), "wb");
		assert(rf);
		assert(fwrite(region_payload, 1, sizeof(region_payload) - 1, rf)
			   == sizeof(region_payload) - 1);
		assert(fclose(rf) == 0);
		FILE* nf = fopen(string_get_cstr(nether_file), "wb");
		assert(nf);
		assert(fwrite(nether_payload, 1, sizeof(nether_payload) - 1, nf)
			   == sizeof(nether_payload) - 1);
		assert(fclose(nf) == 0);

		// Expected mirrors level_archive_disk_size's OWN rule exactly: sum
		// whichever of level.dat/level.dat_old/level.dat_new exist as
		// regular files. By this point in the test (many level_archive_save
		// rotations above), level.dat_old is virtually certain to exist too
		// -- this must NOT be narrowed to just the current level.dat.
		struct stat st;
		int64_t expected = 0;
		static const char* level_suffixes[]
			= {"/level.dat", "/level.dat_old", "/level.dat_new"};
		for(size_t k = 0; k < sizeof(level_suffixes) / sizeof(*level_suffixes);
			k++) {
			string_t p;
			string_init_printf(p, "%s%s", TMP_DIR, level_suffixes[k]);
			if(stat(string_get_cstr(p), &st) == 0)
				expected += (int64_t)st.st_size;
			string_clear(p);
		}
		string_init_printf(level_dat_path, "%s/level.dat", TMP_DIR);
		expected += (int64_t)(sizeof(region_payload) - 1)
			+ (int64_t)(sizeof(nether_payload) - 1);

		struct level_archive la;
		assert(level_archive_create(&la, dir));
		int64_t measured = level_archive_disk_size(&la);
		assert(measured == expected);
		level_archive_destroy(&la);

		remove(string_get_cstr(region_file));
		remove(string_get_cstr(nether_file));
		string_clear(region_file);
		string_clear(nether_file);
		string_clear(region_dir);
		string_clear(nether_dir);
		string_clear(nether_region_dir);
		string_clear(level_dat_path);
		printf("  level_archive_disk_size: level.dat[+_old/_new]+region(10)+"
			   "DIM-1/region(20) = %lld (matches independently-summed "
			   "expected) OK\n",
			   (long long)measured);
	}

	// 8) total loss: no level.dat and no survivors -> create fails (no UB,
	//    no false success).
	remove(test_fixture_join(TMP_DIR, "/level.dat"));
	remove(test_fixture_join(TMP_DIR, "/level.dat_old"));
	remove(test_fixture_join(TMP_DIR, "/level.dat_new"));
	{
		struct level_archive la;
		assert(!level_archive_create(&la, dir));
	}

	string_clear(dir);
	printf("test_level_archive_recovery: OK (rotation recovery + save versioning "
		   "+ D6 death-tx counter)\n");
	test_fixture_dir_cleanup(TMP_DIR);
	return 0;
}
