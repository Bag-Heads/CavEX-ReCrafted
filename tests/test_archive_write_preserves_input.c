#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "support/test_fixture_dir.h"

#include "network/complex_block_archive.h"
#include "network/server_local_state.h" // full server state (split from server_local.h) -- MAX_CHESTS/MAX_CHEST_SLOTS/MAX_SIGNS, struct complex_block_pos by value

// TEST_DIR was a plain literal; CHEST_FILE/CHEST_OLD/... were built on top of
// it via adjacent-string-literal concatenation (CHEST_FILE = TEST_DIR
// "/chests.dat", CHEST_OLD = CHEST_FILE "_old", ...). Once TEST_DIR is a
// per-process runtime buffer that concatenation no longer compiles, so every
// one of these becomes its OWN direct test_fixture_join(TEST_DIR, ...) call
// with the full suffix spelled out (rather than nesting join() calls, e.g.
// CHEST_OLD as test_fixture_join(CHEST_FILE, "_old")) -- nesting compiles
// and is correct (the ring buffer never collides across 2 levels), but once
// GCC inlines it, it sees the destination and a source both pointing into
// the SAME static bufs[][] array and raises a conservative, unfixable-by-
// bounding -Wrestrict warning even though the specific slots never overlap.
// One join() per macro sidesteps that entirely.
static char s_fixture_TEST_DIR[128]; /* was #define TEST_DIR='test_archive_preserve_tmp' (a literal) */
#define TEST_DIR s_fixture_TEST_DIR
#define CHEST_FILE test_fixture_join(TEST_DIR, "/chests.dat")
#define CHEST_OLD test_fixture_join(TEST_DIR, "/chests.dat_old")
#define CHEST_NEW test_fixture_join(TEST_DIR, "/chests.dat_new")
#define SIGN_FILE test_fixture_join(TEST_DIR, "/signs.dat")
#define SIGN_OLD test_fixture_join(TEST_DIR, "/signs.dat_old")
#define SIGN_NEW test_fixture_join(TEST_DIR, "/signs.dat_new")

static struct complex_block_pos s_chest_pos[MAX_CHESTS];
static struct item_data s_chest_items[MAX_CHESTS * MAX_CHEST_SLOTS];
static struct complex_block_pos s_read_chest_pos[MAX_CHESTS];
static struct item_data s_read_chest_items[MAX_CHESTS * MAX_CHEST_SLOTS];
static struct complex_block_pos s_sign_pos[MAX_SIGNS];
static char s_sign_texts[MAX_SIGNS * SIGN_SIZE];
static struct complex_block_pos s_read_sign_pos[MAX_SIGNS];
static char s_read_sign_texts[MAX_SIGNS * SIGN_SIZE];

static void make_dir(const char* dir) {
#ifdef _WIN32
	(void)mkdir(dir);
#else
	(void)mkdir(dir, 0777);
#endif
}

static void cleanup(void) {
	(void)remove(CHEST_FILE);
	(void)remove(CHEST_OLD);
	(void)remove(CHEST_NEW);
	(void)remove(SIGN_FILE);
	(void)remove(SIGN_OLD);
	(void)remove(SIGN_NEW);
	(void)rmdir(TEST_DIR);
}

static void assert_missing(const char* filename) {
	struct stat st;
	assert(stat(filename, &st) != 0);
}

static void fill_chests(int marker) {
	memset(s_chest_pos, -1, sizeof(s_chest_pos));
	memset(s_chest_items, 0, sizeof(s_chest_items));
	s_chest_pos[0] = (struct complex_block_pos) {
		.x = marker,
		.y = marker + 1,
		.z = -marker,
	};
	s_chest_pos[MAX_CHESTS - 1] = (struct complex_block_pos) {
		.x = marker + 2,
		.y = marker + 3,
		.z = -marker - 1,
	};
	s_chest_items[0] = (struct item_data) {
		.id = (uint16_t)(marker + 100),
		.count = 3,
		.durability = (uint16_t)(marker + 200),
	};
	s_chest_items[MAX_CHESTS * MAX_CHEST_SLOTS - 1]
		= (struct item_data) {
			.id = (uint16_t)(marker + 101),
			.count = 4,
			.durability = (uint16_t)(marker + 201),
		};
}

static void assert_chests(int marker) {
	assert(s_read_chest_pos[0].x == marker);
	assert(s_read_chest_pos[MAX_CHESTS - 1].x == marker + 2);
	assert(s_read_chest_items[0].id == marker + 100);
	assert(s_read_chest_items[0].durability == marker + 200);
	assert(s_read_chest_items[MAX_CHESTS * MAX_CHEST_SLOTS - 1].id
		   == marker + 101);
	assert(s_read_chest_items[MAX_CHESTS * MAX_CHEST_SLOTS - 1].durability
		   == marker + 201);
}

static void assert_empty_chests(void) {
	assert(s_read_chest_pos[0].y == -1);
	assert(s_read_chest_pos[MAX_CHESTS - 1].y == -1);
	assert(s_read_chest_items[0].id == 0);
	assert(s_read_chest_items[MAX_CHESTS * MAX_CHEST_SLOTS - 1].id == 0);
}

static void fill_signs(int marker) {
	memset(s_sign_pos, -1, sizeof(s_sign_pos));
	memset(s_sign_texts, ' ', sizeof(s_sign_texts));
	s_sign_pos[0] = (struct complex_block_pos) {
		.x = marker,
		.y = marker + 1,
		.z = -marker,
	};
	s_sign_pos[MAX_SIGNS - 1] = (struct complex_block_pos) {
		.x = marker + 2,
		.y = marker + 3,
		.z = -marker - 1,
	};
	s_sign_texts[0] = (char)('A' + marker / 10);
	s_sign_texts[MAX_SIGNS * SIGN_SIZE - 1] = (char)('a' + marker / 10);
}

static void assert_signs(int marker) {
	assert(s_read_sign_pos[0].x == marker);
	assert(s_read_sign_pos[MAX_SIGNS - 1].x == marker + 2);
	assert(s_read_sign_texts[0] == (char)('A' + marker / 10));
	assert(s_read_sign_texts[MAX_SIGNS * SIGN_SIZE - 1]
		   == (char)('a' + marker / 10));
}

static void assert_empty_signs(void) {
	assert(s_read_sign_pos[0].y == -1);
	assert(s_read_sign_pos[MAX_SIGNS - 1].y == -1);
	assert(s_read_sign_texts[0] == ' ');
	assert(s_read_sign_texts[MAX_SIGNS * SIGN_SIZE - 1] == ' ');
}

static void write_stub(const char* filename, uint8_t version,
					   size_t payload_bytes) {
	FILE* f = fopen(filename, "wb");
	assert(f);
	assert(fputc(version, f) != EOF);
	for(size_t i = 0; i < payload_bytes; i++)
		assert(fputc(0, f) != EOF);
	assert(fclose(f) == 0);
}

static void append_byte(const char* filename) {
	FILE* f = fopen(filename, "ab");
	assert(f);
	assert(fputc(0x5a, f) != EOF);
	assert(fclose(f) == 0);
}

static void assert_file_version(const char* filename, uint8_t version) {
	FILE* f = fopen(filename, "rb");
	assert(f);
	assert(fgetc(f) == version);
	assert(fclose(f) == 0);
}

static void put_be32(uint8_t* out, int32_t v) {
	uint32_t u = (uint32_t)v;
	out[0] = (uint8_t)(u >> 24);
	out[1] = (uint8_t)(u >> 16);
	out[2] = (uint8_t)(u >> 8);
	out[3] = (uint8_t)u;
}

static void put_be16(uint8_t* out, uint16_t v) {
	out[0] = (uint8_t)(v >> 8);
	out[1] = (uint8_t)v;
}

// Independent byte-level oracle for the 2026-09-01 bounded-chunk rewrite of
// chest_archive_write (complex_block_archive.c): the old code malloc'd a
// full MAX_CHESTS position copy (3KB) and a full
// MAX_CHEST_SLOTS*MAX_CHESTS item copy (82,944B) just to hold the
// byte-swapped table before one big fwrite each -- a fragmented-Wii-heap
// save-and-quit abort risk. The rewrite swaps and fwrites one position,
// then one chest's item ROW, at a time from small stack scratch. Every
// position and every slot here gets a DISTINCT value (not the sparse two-
// marker pattern the round-trip checks above use) so a row-transposition
// or off-by-one in the new bounded loops shows up as a byte mismatch here
// even if it happened to round-trip through the (unchanged) reader. The
// expected bytes are computed directly from the documented v1 format
// (big-endian x/y/z int32; big-endian id/durability uint16; raw count
// byte) -- NOT by calling complex_block_archive.c's own swap functions --
// so this is a genuine independent oracle for the on-disk bytes.
static void check_chest_write_golden(void) {
	static struct complex_block_pos pos[MAX_CHESTS];
	static struct item_data items[MAX_CHESTS * MAX_CHEST_SLOTS];
	for(unsigned i = 0; i < MAX_CHESTS; i++)
		pos[i] = (struct complex_block_pos) {
			.x = (int)(i * 3 + 1),
			.y = (int)(1000 - (int)i),
			.z = -(int)(i * 7 + 2),
		};
	for(unsigned i = 0; i < MAX_CHESTS; i++)
		for(unsigned s = 0; s < MAX_CHEST_SLOTS; s++)
			items[i * MAX_CHEST_SLOTS + s] = (struct item_data) {
				.id = (uint16_t)(i * 61 + s + 1),
				.durability = (uint16_t)(i + s * 97),
				.count = (uint8_t)((i + s) % 64 + 1),
			};

	string_t path;
	string_init_set_str(path, TEST_DIR);
	assert(chest_archive_write(pos, items, path, WORLD_DIM_OVERWORLD));
	string_clear(path);

	FILE* f = fopen(CHEST_FILE, "rb");
	assert(f);
	assert(fgetc(f) == 1); // COMPLEX_BLOCK_FORMAT_VERSION (v1)

	for(unsigned i = 0; i < MAX_CHESTS; i++) {
		uint8_t expected[12];
		put_be32(expected, pos[i].x);
		put_be32(expected + 4, pos[i].y);
		put_be32(expected + 8, pos[i].z);
		uint8_t got[12];
		assert(fread(got, sizeof(got), 1, f) == 1);
		assert(memcmp(got, expected, sizeof(got)) == 0
			   && "chest position bytes must match the documented "
				  "big-endian v1 layout, in original index order");
	}

	for(unsigned i = 0; i < MAX_CHESTS; i++) {
		for(unsigned s = 0; s < MAX_CHEST_SLOTS; s++) {
			const struct item_data* it = &items[i * MAX_CHEST_SLOTS + s];
			uint8_t expected[sizeof(struct item_data)];
			memset(expected, 0, sizeof(expected));
			put_be16(expected + offsetof(struct item_data, id), it->id);
			put_be16(expected + offsetof(struct item_data, durability),
					it->durability);
			expected[offsetof(struct item_data, count)] = (uint8_t)it->count;
			uint8_t got[sizeof(struct item_data)];
			assert(fread(got, sizeof(got), 1, f) == 1);
			assert(memcmp(got, expected, sizeof(got)) == 0
				   && "chest item bytes must match the documented "
					  "big-endian v1 layout -- row/slot order preserved by "
					  "the bounded per-chest-row rewrite");
		}
	}

	assert(fgetc(f) == EOF); // exactly the expected length, nothing more
	assert(fclose(f) == 0);
}

// Same independent-oracle treatment for sign_archive_write's bounded
// per-position rewrite. `texts` carries no multi-byte fields and was never
// copied/swapped (unchanged before and after the fix); it is still checked
// here byte-for-byte to prove the position-table rewrite didn't shift its
// placement in the file.
static void check_sign_write_golden(void) {
	static struct complex_block_pos pos[MAX_SIGNS];
	static char texts[MAX_SIGNS * SIGN_SIZE];
	for(unsigned i = 0; i < MAX_SIGNS; i++)
		pos[i] = (struct complex_block_pos) {
			.x = (int)(i * 5 + 3),
			.y = (int)(2000 - (int)i),
			.z = -(int)(i * 11 + 4),
		};
	for(unsigned i = 0; i < MAX_SIGNS * SIGN_SIZE; i++)
		texts[i] = (char)('a' + (i % 26));

	string_t path;
	string_init_set_str(path, TEST_DIR);
	assert(sign_archive_write(pos, texts, path, WORLD_DIM_OVERWORLD));
	string_clear(path);

	FILE* f = fopen(SIGN_FILE, "rb");
	assert(f);
	assert(fgetc(f) == 0); // sign format version byte is always 0

	for(unsigned i = 0; i < MAX_SIGNS; i++) {
		uint8_t expected[12];
		put_be32(expected, pos[i].x);
		put_be32(expected + 4, pos[i].y);
		put_be32(expected + 8, pos[i].z);
		uint8_t got[12];
		assert(fread(got, sizeof(got), 1, f) == 1);
		assert(memcmp(got, expected, sizeof(got)) == 0
			   && "sign position bytes must match the big-endian v1 layout, "
				  "in original index order");
	}

	char got_texts[MAX_SIGNS * SIGN_SIZE];
	assert(fread(got_texts, sizeof(got_texts), 1, f) == 1);
	assert(memcmp(got_texts, texts, sizeof(got_texts)) == 0
		   && "sign text bytes are raw/unswapped and must be byte-identical");

	assert(fgetc(f) == EOF);
	assert(fclose(f) == 0);
}

int main(void) {
	test_fixture_dir("test_archive_preserve_tmp", s_fixture_TEST_DIR, sizeof s_fixture_TEST_DIR);
	cleanup(); // cleanup()/make_dir() both already ran at start+end before this
	           // change too -- test_fixture_dir()'s own defensive wipe above
	           // makes the cleanup() call here belt-and-suspenders, not load-bearing.
	make_dir(TEST_DIR);

	string_t path;
	string_init_set_str(path, TEST_DIR);

	/* Writes use private endian-swapped copies rather than mutating live data. */
	fill_chests(10);
	fill_signs(10);
	assert(chest_archive_write(
		s_chest_pos, s_chest_items, path, WORLD_DIM_OVERWORLD));
	assert(sign_archive_write(
		s_sign_pos, s_sign_texts, path, WORLD_DIM_OVERWORLD));
	assert(s_chest_pos[0].x == 10);
	assert(s_chest_items[0].id == 110);
	assert(s_sign_pos[0].x == 10);
	assert(chest_archive_read(
		s_read_chest_pos, s_read_chest_items, path, WORLD_DIM_OVERWORLD));
	assert_chests(10);

	/* The first failed promotion moves the current primary to `_old`; the next
	 * failure must not delete that sole valid archive. */
	fill_chests(20);
	assert(chest_archive_write(
		s_chest_pos, s_chest_items, path, WORLD_DIM_OVERWORLD));
	complex_block_archive_test_fail_promotions(2);
	fill_chests(30);
	assert(!chest_archive_write(
		s_chest_pos, s_chest_items, path, WORLD_DIM_OVERWORLD));
	fill_chests(40);
	assert(!chest_archive_write(
		s_chest_pos, s_chest_items, path, WORLD_DIM_OVERWORLD));
	assert(chest_archive_read(
		s_read_chest_pos, s_read_chest_items, path, WORLD_DIM_OVERWORLD));
	assert_chests(20);

	/* A failed sync after `_new` takes the committed name leaves the new
	 * primary uncertain and `_old` as the last proven snapshot. A retry whose
	 * primary pre-sync also fails must not consume that backup; a later healthy
	 * retry can proceed normally. */
	fill_chests(30);
	assert(chest_archive_write(
		s_chest_pos, s_chest_items, path, WORLD_DIM_OVERWORLD));
	complex_block_archive_test_fail_sync_after(1);
	fill_chests(40);
	assert(!chest_archive_write(
		s_chest_pos, s_chest_items, path, WORLD_DIM_OVERWORLD));
	complex_block_archive_test_fail_sync_after(0);
	fill_chests(50);
	assert(!chest_archive_write(
		s_chest_pos, s_chest_items, path, WORLD_DIM_OVERWORLD));
	assert(remove(CHEST_FILE) == 0);
	assert(chest_archive_read(
		s_read_chest_pos, s_read_chest_items, path, WORLD_DIM_OVERWORLD));
	assert_chests(30);
	assert(chest_archive_write(
		s_chest_pos, s_chest_items, path, WORLD_DIM_OVERWORLD));
	assert(chest_archive_read(
		s_read_chest_pos, s_read_chest_items, path, WORLD_DIM_OVERWORLD));
	assert_chests(50);

	/* Truncated and missing primaries use the validated fallback without
	 * promoting or deleting the sole good copy. */
	write_stub(CHEST_FILE, 1, 7);
	assert(chest_archive_read(
		s_read_chest_pos, s_read_chest_items, path, WORLD_DIM_OVERWORLD));
	assert_chests(30);
	assert_missing(CHEST_FILE);
	assert(chest_archive_read(
		s_read_chest_pos, s_read_chest_items, path, WORLD_DIM_OVERWORLD));
	assert_chests(30);

	/* A newer schema is not corruption and must never be replaced by a stale
	 * backup. Fail closed while preserving both files for the newer binary. */
	write_stub(CHEST_FILE, 0x7f, 0);
	assert(!chest_archive_read(
		s_read_chest_pos, s_read_chest_items, path, WORLD_DIM_OVERWORLD));
	assert_empty_chests();
	assert_file_version(CHEST_FILE, 0x7f);
	assert_file_version(CHEST_OLD, 1);
	assert(remove(CHEST_FILE) == 0);
	assert(remove(CHEST_OLD) == 0);
	write_stub(CHEST_FILE, 1, 7);
	assert(!chest_archive_read(
		s_read_chest_pos, s_read_chest_items, path, WORLD_DIM_OVERWORLD));
	assert_empty_chests();
	assert(remove(CHEST_FILE) == 0);
	assert(chest_archive_read(
		s_read_chest_pos, s_read_chest_items, path, WORLD_DIM_OVERWORLD));
	assert_empty_chests();

	/* Extra trailing bytes are corruption, not a valid prefix. */
	fill_chests(50);
	assert(chest_archive_write(
		s_chest_pos, s_chest_items, path, WORLD_DIM_OVERWORLD));
	append_byte(CHEST_FILE);
	assert(!chest_archive_read(
		s_read_chest_pos, s_read_chest_items, path, WORLD_DIM_OVERWORLD));
	assert_empty_chests();

	/* Signs share the same exact-read and fallback contract. */
	fill_signs(20);
	assert(sign_archive_write(
		s_sign_pos, s_sign_texts, path, WORLD_DIM_OVERWORLD));
	assert(remove(SIGN_FILE) == 0);
	assert(sign_archive_read(
		s_read_sign_pos, s_read_sign_texts, path, WORLD_DIM_OVERWORLD));
	assert_signs(10);
	write_stub(SIGN_FILE, 0, 5);
	assert(sign_archive_read(
		s_read_sign_pos, s_read_sign_texts, path, WORLD_DIM_OVERWORLD));
	assert_signs(10);
	write_stub(SIGN_FILE, 1, 0);
	assert(!sign_archive_read(
		s_read_sign_pos, s_read_sign_texts, path, WORLD_DIM_OVERWORLD));
	assert_empty_signs();
	assert_file_version(SIGN_FILE, 1);
	assert_file_version(SIGN_OLD, 0);
	assert(remove(SIGN_FILE) == 0);
	assert(remove(SIGN_OLD) == 0);
	write_stub(SIGN_FILE, 0, 5);
	assert(!sign_archive_read(
		s_read_sign_pos, s_read_sign_texts, path, WORLD_DIM_OVERWORLD));
	assert_empty_signs();
	assert(remove(SIGN_FILE) == 0);
	assert(sign_archive_read(
		s_read_sign_pos, s_read_sign_texts, path, WORLD_DIM_OVERWORLD));
	assert_empty_signs();

	/* 2026-09-01 bounded-chunk write rewrite: byte-level golden proof (not
	 * just round-trip) that the new per-position/per-chest-row stack-scratch
	 * loops emit exactly the same file bytes the old two-big-heap-copy code
	 * did. */
	check_chest_write_golden();
	check_sign_write_golden();

	string_clear(path);
	cleanup();
	return 0;
}
