/*
	Copyright (c) 2022 ByteBit/xtreme8000

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	CavEX is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with CavEX.  If not, see <http://www.gnu.org/licenses/>.
*/

/* Strict -std=c99 hides fileno unless POSIX declarations are requested before
 * any system header. Wii libfat implements fsync through FatFs f_sync. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "complex_block_archive.h"
#include "server_local_state.h" // full server state (split from server_local.h) -- struct complex_block_pos by value
#include "tile_entities_store.h" // A1b: world-selected te_store views

static int32_t swap_int32(int32_t n) {
	// Swap in UNSIGNED arithmetic: a signed left-shift of a negative value (e.g. a chest at a
	// negative coordinate) is UB (caught by UBSan in the soak). The bit pattern is identical to the
	// old signed form -- the masks already discarded the sign-extended bits -- so the swap result is
	// byte-for-byte unchanged; only the undefined behaviour is removed.
	uint32_t u = (uint32_t)n;
	u = ((u << 8) & 0xFF00FF00u) | ((u >> 8) & 0x00FF00FFu);
	return (int32_t)((u << 16) | (u >> 16));
}

static uint16_t swap_uint16(uint16_t n) {
	return (n << 8) | (n >> 8);
}

static int is_little_endian() {
	volatile uint32_t i=0x01234567;
	return (*((uint8_t*)(&i))) == 0x67;
}

// FORMAT VERSIONS (the leading byte): v0 swapped only item ids -- the
// uint16-widened durability field was serialized in NATIVE order, so a
// chests.dat written on PC (LE) read tool durabilities byte-swapped on the
// Wii (BE) and vice versa (the durability-uint16 fix's missed sibling,
// 2026-06-12 audit). v1 swaps durability too (file canonically big-endian).
// Reads honor the version byte so existing v0 files keep their original
// (same-platform-roundtrip) semantics instead of being mis-swapped.
#define COMPLEX_BLOCK_FORMAT_VERSION 1

#ifdef CAVEWII_COMPLEX_BLOCK_ARCHIVE_TEST
static bool s_test_sync_failure_armed;
static unsigned s_test_successful_syncs_before_failure;

void complex_block_archive_test_fail_sync_after(unsigned successful_syncs) {
	s_test_sync_failure_armed = true;
	s_test_successful_syncs_before_failure = successful_syncs;
}

static bool cba_test_sync_allowed(void) {
	if(!s_test_sync_failure_armed)
		return true;
	if(s_test_successful_syncs_before_failure > 0) {
		s_test_successful_syncs_before_failure--;
		return true;
	}
	s_test_sync_failure_armed = false;
	return false;
}
#else
#define cba_test_sync_allowed() true
#endif

static bool cba_file_sync(FILE* f) {
	if(!f || fflush(f) != 0)
		return false;
#ifdef _WIN32
	return _commit(_fileno(f)) == 0;
#else
	return fsync(fileno(f)) == 0;
#endif
}

static bool cba_sync_path(const char* path) {
	if(!cba_test_sync_allowed())
		return false;
	FILE* f = fopen(path, "rb+");
	if(!f)
		return false;
	bool ok = cba_file_sync(f);
	if(fclose(f) != 0)
		ok = false;
	return ok;
}

static bool cba_remove_if_present(const char* path) {
	if(remove(path) == 0)
		return true;
	return errno == ENOENT;
}

static bool cba_path_status(const char* path, bool* exists) {
	struct stat st;
	errno = 0;
	if(stat(path, &st) == 0) {
		*exists = true;
		return true;
	}
	*exists = false;
	return errno == ENOENT;
}

#ifdef CAVEWII_COMPLEX_BLOCK_ARCHIVE_TEST
static unsigned s_test_promotion_failures;

void complex_block_archive_test_fail_promotions(unsigned count) {
	s_test_promotion_failures = count;
}

static bool cba_promote(const char* fresh, const char* final) {
	if(s_test_promotion_failures > 0) {
		s_test_promotion_failures--;
		return false;
	}
	return rename(fresh, final) == 0;
}
#else
static bool cba_promote(const char* fresh, const char* final) {
	return rename(fresh, final) == 0;
}
#endif

// Replace `final` with the freshly synced `_new`. If a previous failed
// promotion left only `_old`, preserve it until the new primary exists; deleting
// that sole valid backup made two consecutive failures destructive.
static bool cba_rotate_into_place(const char* final) {
	string_t tmp_name, old_name;
	string_init_printf(tmp_name, "%s_new", final);
	string_init_printf(old_name, "%s_old", final);

	bool final_exists = false;
	bool promoted = cba_path_status(final, &final_exists);
	/* A prior attempt can have promoted `_new` and then failed its committed-
	 * name sync, leaving an uncertain final beside the last proven `_old`.
	 * Prove every existing final before any destructive rotation. On failure
	 * both committed copies remain untouched and only this attempt's `_new` is
	 * discarded. */
	if(promoted && final_exists)
		promoted = cba_sync_path(final);
	if(promoted && final_exists) {
		promoted = cba_remove_if_present(string_get_cstr(old_name));
		if(promoted)
			promoted = rename(final, string_get_cstr(old_name)) == 0;
	}
	if(promoted)
		promoted = cba_promote(string_get_cstr(tmp_name), final);
	if(promoted)
		promoted = cba_sync_path(final);
	if(!promoted)
		(void)cba_remove_if_present(string_get_cstr(tmp_name));

	string_clear(tmp_name);
	string_clear(old_name);
	return promoted;
}

#ifdef CAVEWII_COMPLEX_BLOCK_ARCHIVE_TEST
static bool s_test_write_failure_armed;
static unsigned s_test_successful_writes_before_failure;

void complex_block_archive_test_fail_write_after(unsigned successful_writes) {
	s_test_write_failure_armed = true;
	s_test_successful_writes_before_failure = successful_writes;
}

static bool cba_test_write_allowed(void) {
	if(!s_test_write_failure_armed)
		return true;
	if(s_test_successful_writes_before_failure > 0) {
		s_test_successful_writes_before_failure--;
		return true;
	}
	s_test_write_failure_armed = false;
	return false;
}
#else
#define cba_test_write_allowed() true
#endif

enum cba_read_result {
	CBA_READ_ABSENT,
	CBA_READ_VALID,
	CBA_READ_INVALID,
	CBA_READ_UNSUPPORTED_VERSION,
};

static enum cba_read_result cba_read_exact(
	const char* filename, uint8_t max_version, void* first, size_t first_bytes,
	void* second, size_t second_bytes, uint8_t* version_out) {
	errno = 0;
	FILE* f = fopen(filename, "rb");
	if(!f)
		return errno == ENOENT ? CBA_READ_ABSENT : CBA_READ_INVALID;

	int version = fgetc(f);
	if(version > max_version) {
		/* Once the leading byte proves this belongs to a newer schema, no
		 * subsequent I/O/close error may reclassify it as deletable corruption. */
		(void)fclose(f);
		return CBA_READ_UNSUPPORTED_VERSION;
	}
	bool ok = version >= 0
		&& fread(first, 1, first_bytes, f) == first_bytes
		&& (!second_bytes
			|| fread(second, 1, second_bytes, f) == second_bytes);
	if(ok) {
		int trailing = fgetc(f);
		ok = trailing == EOF && feof(f) && !ferror(f);
	}
	if(fclose(f) != 0)
		ok = false;
	if(!ok)
		return CBA_READ_INVALID;
	*version_out = (uint8_t)version;
	return CBA_READ_VALID;
}

/* Keep the validated fallback in `_old`. Removing an invalid primary makes the
 * next rotation enter its final-absent path, which preserves this sole backup
 * until `_new` has promoted successfully. */
static bool cba_prepare_old_fallback(const char* final,
								 enum cba_read_result primary) {
	return primary != CBA_READ_INVALID || cba_remove_if_present(final);
}

static void swap_endianness(struct complex_block_pos* pos, size_t pos_count,
							struct item_data* items, size_t item_count,
							bool swap_durability) {
	if(is_little_endian()) {
		for(size_t i=0; i<pos_count; i++) {
			pos[i].x = swap_int32(pos[i].x);
			pos[i].y = swap_int32(pos[i].y);
			pos[i].z = swap_int32(pos[i].z);
		}

		for(size_t i=0; items && i<item_count; i++) {
			items[i].id = swap_uint16(items[i].id);
			if(swap_durability)
				items[i].durability = swap_uint16(items[i].durability);
		}
	}
}

// M5: chests.dat / signs.dat were dimension-BLIND flat tables keyed only by
// (x,y,z) -- after a live dimension switch the RAM table would seed from the
// OTHER dimension's file and te_store_gather (chunk-coord keyed) would bleed
// dim-A chests into dim-B chunk NBT. Nether state now lives in sibling files
// (chests_dim-1.dat / signs_dim-1.dat); overworld paths are UNCHANGED, so
// existing saves keep working.
static const char* cba_chest_file(enum world_dim dim) {
	return dim == WORLD_DIM_NETHER ? "chests_dim-1.dat" : "chests.dat";
}
static const char* cba_sign_file(enum world_dim dim) {
	return dim == WORLD_DIM_NETHER ? "signs_dim-1.dat" : "signs.dat";
}

/* A1b: is chest/sign row i an OCCUPIED row of ANOTHER world than `st`'s?
 * Such a row is written as an empty row (position -1, zero payload) so a
 * world's flat archive never captures the other world's tiles. Free rows and
 * this world's rows are written exactly as before, so a single-world table
 * produces the byte-identical file (test_archive_write_preserves_input). */
static bool cba_foreign_row(const struct te_store* st, enum te_kind kind,
							const struct complex_block_pos* pos, size_t i) {
	return st && pos[i].y >= 0 && !te_store_row_owned(st, kind, (int)i);
}

static bool chest_archive_write_impl(const struct te_store* st,
									 struct complex_block_pos* pos,
									 struct item_data* items, string_t path,
									 enum world_dim dim) {
	assert(pos && items && path);
	/* The legacy six-byte flat format cannot represent opaque NBT. */
	for(size_t k=0;k<MAX_CHESTS*MAX_CHEST_SLOTS;k++)
		if(items[k].nbt_record
		   && !cba_foreign_row(st, TE_KIND_CHEST, pos, k / MAX_CHEST_SLOTS))
			return false;
	if(!cba_test_write_allowed())
		return false;

	// Write to chests.dat_new, then rotate into place (crash-safe; see
	// cba_rotate_into_place). The old code truncated chests.dat in place.
	string_t final, tmp;
	string_init_printf(final, "%s/%s", string_get_cstr(path), cba_chest_file(dim));
	string_init_printf(tmp, "%s_new", string_get_cstr(final));
	FILE* f = fopen(string_get_cstr(tmp), "wb");
	if (!f) {
		string_clear(final);
		string_clear(tmp);
		return false;
	}

	bool ok = fputc(COMPLEX_BLOCK_FORMAT_VERSION, f) != EOF; //format version byte

	// T1 (2026-09-01, fragmented-Wii-heap save-and-quit abort): the old code
	// malloc'd a MAX_CHESTS-sized (3KB) position copy AND a
	// MAX_CHEST_SLOTS*MAX_CHESTS-sized (82,944B) item copy just to hold the
	// byte-swapped table before one big fwrite each. Neither allocation is
	// needed: byte-swap and fwrite ONE position at a time (a 12-byte stack
	// scratch, reused every iteration) -- MAX_CHESTS single-position writes,
	// each immediately after swapping, land in the file in the exact same
	// order the old single fwrite of the fully-swapped copy did, so the
	// bytes on disk are unchanged. See check_chest_write_golden in
	// test_archive_write_preserves_input.c for the byte-identical-output
	// proof (an independent big-endian oracle, not a round-trip check).
	for(size_t i = 0; ok && i < MAX_CHESTS; i++) {
		struct complex_block_pos p = pos[i];
		if(cba_foreign_row(st, TE_KIND_CHEST, pos, i))
			p = (struct complex_block_pos) {-1, -1, -1};
		swap_endianness(&p, 1, NULL, 0, false);
		ok = fwrite(&p, sizeof(p), 1, f) == 1;
	}

	// Same per-CHEST-ROW treatment for the item table: one chest's
	// MAX_CHEST_SLOTS (54) items -- 324 bytes -- in a stack buffer at a time,
	// instead of the full 82,944-byte heap copy. Rows are contiguous and
	// written in the same order the old code's single big fwrite covered
	// them in, so MAX_CHESTS row-writes reproduce the identical byte stream.
	for(size_t i = 0; ok && i < MAX_CHESTS; i++) {
		struct item_data row[MAX_CHEST_SLOTS];
		if(cba_foreign_row(st, TE_KIND_CHEST, pos, i))
			memset(row, 0, sizeof(row));
		else
			memcpy(row, items + i * MAX_CHEST_SLOTS, sizeof(row));
		swap_endianness(NULL, 0, row, MAX_CHEST_SLOTS, true);
		ok = fwrite(row, sizeof(row), 1, f) == 1;
	}

	if(ok && !cba_file_sync(f))
		ok = false;
	if(fclose(f) != 0) // close failure also surfaces here
		ok = false;

	if(ok)
		ok = cba_rotate_into_place(string_get_cstr(final));
	else
		(void)cba_remove_if_present(
			string_get_cstr(tmp)); // partial write: keep committed copies

	string_clear(final);
	string_clear(tmp);
	return ok;
}

bool chest_archive_write(struct complex_block_pos* pos, struct item_data* items, string_t path,
						 enum world_dim dim) {
	return chest_archive_write_impl(NULL, pos, items, path, dim);
}

bool chest_archive_read(struct complex_block_pos* pos, struct item_data* items, string_t path,
						enum world_dim dim) {
	assert(pos && items && path);
	memset(pos, -1, MAX_CHESTS*sizeof(struct complex_block_pos));
	memset(items, 0, MAX_CHEST_SLOTS*MAX_CHESTS*sizeof(struct item_data));

	string_t filename, old_name;
	string_init_printf(filename, "%s/%s", string_get_cstr(path),
					   cba_chest_file(dim));
	string_init_printf(old_name, "%s_old", string_get_cstr(filename));
	uint8_t format_version = 0;
	enum cba_read_result primary = cba_read_exact(
		string_get_cstr(filename), COMPLEX_BLOCK_FORMAT_VERSION,
		pos, MAX_CHESTS * sizeof(struct complex_block_pos), items,
		MAX_CHEST_SLOTS * MAX_CHESTS * sizeof(struct item_data),
		&format_version);
	if(primary == CBA_READ_UNSUPPORTED_VERSION) {
		string_clear(filename);
		string_clear(old_name);
		return false;
	}
	if(primary != CBA_READ_VALID) {
		memset(pos, -1, MAX_CHESTS * sizeof(struct complex_block_pos));
		memset(items, 0,
			   MAX_CHEST_SLOTS * MAX_CHESTS * sizeof(struct item_data));
		enum cba_read_result backup = cba_read_exact(
			string_get_cstr(old_name), COMPLEX_BLOCK_FORMAT_VERSION,
			pos, MAX_CHESTS * sizeof(struct complex_block_pos), items,
			MAX_CHEST_SLOTS * MAX_CHESTS * sizeof(struct item_data),
			&format_version);
		if(backup == CBA_READ_VALID) {
			if(!cba_prepare_old_fallback(
				   string_get_cstr(filename), primary)) {
				memset(pos, -1,
					   MAX_CHESTS * sizeof(struct complex_block_pos));
				memset(items, 0, MAX_CHEST_SLOTS * MAX_CHESTS
								  * sizeof(struct item_data));
				string_clear(filename);
				string_clear(old_name);
				return false;
			}
		} else if(primary == CBA_READ_ABSENT
				  && backup == CBA_READ_ABSENT) {
			string_clear(filename);
			string_clear(old_name);
			return true;
		} else {
			memset(pos, -1,
				   MAX_CHESTS * sizeof(struct complex_block_pos));
			memset(items, 0, MAX_CHEST_SLOTS * MAX_CHESTS
							  * sizeof(struct item_data));
			string_clear(filename);
			string_clear(old_name);
			return false;
		}
	}
	string_clear(filename);
	string_clear(old_name);

	// v0 files carry durability in writer-native order: swapping it here
	// would corrupt a same-platform roundtrip, so only v1+ swaps it.
	/* The former padding byte in v0/v1 was never a persistent handle. */
	for(size_t k=0;k<MAX_CHESTS*MAX_CHEST_SLOTS;k++) items[k].nbt_record=0;
	swap_endianness(pos, MAX_CHESTS, items, MAX_CHEST_SLOTS * MAX_CHESTS,
					format_version >= 1);
	return true;
}

static bool sign_archive_write_impl(const struct te_store* st,
									struct complex_block_pos* pos, char* texts,
									string_t path, enum world_dim dim) {
	assert(pos && texts && path);
	if(!cba_test_write_allowed())
		return false;

	// Crash-safe rotation, same as chests.dat above.
	string_t final, tmp;
	string_init_printf(final, "%s/%s", string_get_cstr(path), cba_sign_file(dim));
	string_init_printf(tmp, "%s_new", string_get_cstr(final));
	FILE* f = fopen(string_get_cstr(tmp), "wb");
	if (!f) {
		string_clear(final);
		string_clear(tmp);
		return false;
	}

	bool ok = fputc(0, f) != EOF; //format version byte

	// T1 (2026-09-01): same per-position stack-scratch treatment as
	// chest_archive_write above -- no MAX_SIGNS-sized (3KB) heap copy needed
	// just to hold the byte-swapped position table for one big fwrite.
	// `texts` carries no multi-byte fields (raw sign-line bytes), so it never
	// needed a swap copy and is unchanged: one fwrite straight from the
	// caller's buffer, in the same position after the position table.
	for(size_t i = 0; ok && i < MAX_SIGNS; i++) {
		struct complex_block_pos p = pos[i];
		if(cba_foreign_row(st, TE_KIND_SIGN, pos, i))
			p = (struct complex_block_pos) {-1, -1, -1};
		swap_endianness(&p, 1, NULL, 0, false);
		ok = fwrite(&p, sizeof(p), 1, f) == 1;
	}

	/* Unmasked: one fwrite straight from the caller's buffer (unchanged).
	 * Masked: row by row, blanking another world's text to the space-padded
	 * empty-sign image. */
	if(!st) {
		ok = ok && fwrite(texts, 1, MAX_SIGNS*SIGN_SIZE*sizeof(char), f)
			== MAX_SIGNS*SIGN_SIZE*sizeof(char);
	} else {
		for(size_t i = 0; ok && i < MAX_SIGNS; i++) {
			char row[SIGN_SIZE];
			if(cba_foreign_row(st, TE_KIND_SIGN, pos, i))
				memset(row, ' ', sizeof(row));
			else
				memcpy(row, texts + i * SIGN_SIZE, sizeof(row));
			ok = fwrite(row, 1, sizeof(row), f) == sizeof(row);
		}
	}
	if(ok && !cba_file_sync(f))
		ok = false;
	if(fclose(f) != 0)
		ok = false;

	if(ok)
		ok = cba_rotate_into_place(string_get_cstr(final));
	else
		(void)cba_remove_if_present(string_get_cstr(tmp));

	string_clear(final);
	string_clear(tmp);
	return ok;
}

bool sign_archive_write(struct complex_block_pos* pos, char* texts, string_t path,
						enum world_dim dim) {
	return sign_archive_write_impl(NULL, pos, texts, path, dim);
}

bool sign_archive_read(struct complex_block_pos* pos, char* texts, string_t path,
					   enum world_dim dim) {
	assert(pos && texts && path);
	memset(pos, -1, MAX_SIGNS*sizeof(struct complex_block_pos));
	memset(texts, ' ', MAX_SIGNS*SIGN_SIZE*sizeof(char));

	string_t filename, old_name;
	string_init_printf(filename, "%s/%s", string_get_cstr(path),
					   cba_sign_file(dim));
	string_init_printf(old_name, "%s_old", string_get_cstr(filename));
	uint8_t format_version = 0;
	enum cba_read_result primary = cba_read_exact(
		string_get_cstr(filename), 0, pos,
		MAX_SIGNS * sizeof(struct complex_block_pos), texts,
		MAX_SIGNS * SIGN_SIZE * sizeof(char), &format_version);
	if(primary == CBA_READ_UNSUPPORTED_VERSION) {
		string_clear(filename);
		string_clear(old_name);
		return false;
	}
	if(primary != CBA_READ_VALID) {
		memset(pos, -1, MAX_SIGNS * sizeof(struct complex_block_pos));
		memset(texts, ' ', MAX_SIGNS * SIGN_SIZE * sizeof(char));
		enum cba_read_result backup = cba_read_exact(
			string_get_cstr(old_name), 0, pos,
			MAX_SIGNS * sizeof(struct complex_block_pos), texts,
			MAX_SIGNS * SIGN_SIZE * sizeof(char), &format_version);
		if(backup == CBA_READ_VALID) {
			if(!cba_prepare_old_fallback(
				   string_get_cstr(filename), primary)) {
				memset(pos, -1,
					   MAX_SIGNS * sizeof(struct complex_block_pos));
				memset(texts, ' ', MAX_SIGNS * SIGN_SIZE * sizeof(char));
				string_clear(filename);
				string_clear(old_name);
				return false;
			}
		} else if(primary == CBA_READ_ABSENT
				  && backup == CBA_READ_ABSENT) {
			string_clear(filename);
			string_clear(old_name);
			return true;
		} else {
			memset(pos, -1,
				   MAX_SIGNS * sizeof(struct complex_block_pos));
			memset(texts, ' ', MAX_SIGNS * SIGN_SIZE * sizeof(char));
			string_clear(filename);
			string_clear(old_name);
			return false;
		}
	}
	string_clear(filename);
	string_clear(old_name);

	swap_endianness(pos, MAX_SIGNS, NULL, 0, false);
	return true;
}

/* ===== A1b world-selected legacy archives ================================ */

static bool cba_table_has_foreign(const struct te_store* st,
								  enum te_kind kind,
								  const struct complex_block_pos* pos,
								  size_t rows) {
	for(size_t i = 0; i < rows; i++)
		if(cba_foreign_row(st, kind, pos, i))
			return true;
	return false;
}

static void cba_stamp_all(const struct te_store* st, enum te_kind kind,
						  struct complex_block_pos* pos, size_t rows) {
	const struct te_owner_view* o = &st->owners[kind];
	if(!st->owner_keyed || !o->slot || !o->generation)
		return;
	for(size_t i = 0; i < rows; i++) {
		bool occupied = pos[i].y >= 0; /* free rows are world-neutral {0,0} */
		o->slot[i] = occupied ? st->owner_slot : 0;
		o->generation[i] = occupied ? st->owner_generation : 0;
	}
}

bool chest_archive_write_w(const struct te_store* st, string_t path,
						   enum world_dim dim) {
	if(!st || !st->owner_keyed || !st->chest_pos || !st->chest_items)
		return false;
	return chest_archive_write_impl(st, (struct complex_block_pos*)st->chest_pos,
									st->chest_items, path, dim);
}

bool sign_archive_write_w(const struct te_store* st, string_t path,
						  enum world_dim dim) {
	if(!st || !st->owner_keyed || !st->sign_pos || !st->sign_texts)
		return false;
	return sign_archive_write_impl(st, (struct complex_block_pos*)st->sign_pos,
								   st->sign_texts, path, dim);
}

enum storage_rc complex_archive_prepare_w(const struct te_store* st,
										  string_t path, enum world_dim dim,
										  struct tile_restore_plan* plan) {
	if(!st || !plan || !path)
		return STORAGE_IO_ERROR;
	memset(plan, 0, sizeof(*plan));
	plan->dim = dim;
	plan->chest_max = MAX_CHESTS;
	plan->chest_slots = MAX_CHEST_SLOTS;
	plan->sign_max = MAX_SIGNS;
	plan->sign_size = SIGN_SIZE;
	plan->chest_pos = te_restore_alloc(sizeof(struct te_pos) * MAX_CHESTS);
	plan->chest_items = te_restore_alloc(sizeof(struct item_data) * MAX_CHESTS
										 * MAX_CHEST_SLOTS);
	plan->sign_pos = te_restore_alloc(sizeof(struct te_pos) * MAX_SIGNS);
	plan->sign_texts = te_restore_alloc((size_t)MAX_SIGNS * SIGN_SIZE);
	plan->chest_dest = te_restore_alloc(sizeof(int16_t) * MAX_CHESTS);
	plan->sign_dest = te_restore_alloc(sizeof(int16_t) * MAX_SIGNS);
	if(!plan->chest_pos || !plan->chest_items || !plan->sign_pos
	   || !plan->sign_texts || !plan->chest_dest || !plan->sign_dest) {
		te_restore_abort(plan);
		return STORAGE_CAPACITY;
	}
	/* The SAME decoders as before (version/endian rules, 54 slots, sign
	 * bytes, the `_old` fallback) -- into scratch. */
	if(!chest_archive_read((struct complex_block_pos*)plan->chest_pos,
						   plan->chest_items, path, dim)
	   || !sign_archive_read((struct complex_block_pos*)plan->sign_pos,
							 plan->sign_texts, path, dim)) {
		te_restore_abort(plan);
		return STORAGE_CORRUPT;
	}
	plan->prepared = true;
	return STORAGE_OK;
}

/* One-kind wrapper: the other kind's image is emptied so the plan installs
 * nothing for it -- but its rows would be RELEASED by the commit's wipe, so
 * the wrapper snapshots them first and reinstalls them after. */
static bool cba_read_one_kind_w(const struct te_store* st, string_t path,
								enum world_dim dim, enum te_kind keep_kind) {
	if(!st || !st->owner_keyed || !st->chest_pos || !st->chest_items
	   || !st->sign_pos || !st->sign_texts)
		return false;
	struct tile_restore_plan plan;
	if(complex_archive_prepare_w(st, path, dim, &plan) != STORAGE_OK)
		return false;
	/* Replace the OTHER kind's incoming image with the table's own current
	 * LIVE rows for this view, so the commit re-installs them unchanged. Rows
	 * stamped by a previous generation of this slot are not carried: the
	 * commit releases them (R2-H1/R2-L2 -- no per-reload accumulation). */
	if(keep_kind == TE_KIND_CHEST) {
		for(int i = 0; i < plan.sign_max; i++) {
			plan.sign_pos[i] = (struct te_pos) {-1, -1, -1};
			if(te_live(st, TE_KIND_SIGN, st->sign_pos, i)) {
				plan.sign_pos[i] = st->sign_pos[i];
				memcpy(plan.sign_texts + (size_t)i * plan.sign_size,
					   st->sign_texts + (size_t)i * st->sign_size,
					   (size_t)plan.sign_size);
			}
		}
	} else {
		for(int i = 0; i < plan.chest_max; i++) {
			plan.chest_pos[i] = (struct te_pos) {-1, -1, -1};
			if(te_live(st, TE_KIND_CHEST, st->chest_pos, i)) {
				plan.chest_pos[i] = st->chest_pos[i];
				memcpy(plan.chest_items + (size_t)i * plan.chest_slots,
					   st->chest_items + (size_t)i * st->chest_slots,
					   (size_t)plan.chest_slots * sizeof(struct item_data));
			}
		}
	}
	if(te_restore_preflight(st, &plan) != STORAGE_OK) {
		te_restore_abort(&plan);
		return false;
	}
	return te_restore_commit(&plan);
}

bool chest_archive_read_w(const struct te_store* st, string_t path,
						  enum world_dim dim) {
	return cba_read_one_kind_w(st, path, dim, TE_KIND_CHEST);
}

bool sign_archive_read_w(const struct te_store* st, string_t path,
						 enum world_dim dim) {
	return cba_read_one_kind_w(st, path, dim, TE_KIND_SIGN);
}
