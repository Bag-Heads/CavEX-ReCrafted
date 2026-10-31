/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

// Beta MapStorage.getUniqueDataId/loadIdCounts [740c583], faithfully
// transcribed as RAW (non-gzip) NBT -- see map_idcounts.h's header comment
// for the decompile citation of that (surprising) format choice.
//
// Durability note: this module uses the same temp-file + fsync + atomic-
// rename write map_data_archive.c/level_archive.c/death_drop_journal.c all
// share, so a crash or power cut can never leave a torn/partial idcounts.dat
// behind -- the committed file is always either the OLD complete table or
// the NEW complete table, never a mix.
//
// CORRECTNESS-AUDIT FIX (was: "deliberately does NOT add map_data_archive
// .c's extra '_new' crash-window recovery fallback"): that scope reduction
// was wrong. The window it waved off -- dying between the pre-rename
// `remove(path)` and the `rename()` itself -- does not merely lose ONE id
// allocation; it deletes idcounts.dat outright, so the NEXT call (for a
// completely unrelated, already-existing map) recomputes id 0 from an empty
// table and map_idcounts_write_entries overwrites that map's live
// map_N.dat/cache entry. That is not self-healing, it is silent data loss.
// This module now carries the exact same "_new" fallback map_data_archive.c
// does (map_idcounts_load_entries_path below mirrors
// map_data_archive_load_path/map_data_archive_load 1:1, including the
// present-but-corrupt-must-fail-closed distinction and the self-heal
// promotion), and map_idcounts_write_entries never deletes a just-written,
// fsynced "_new" survivor merely because the promoting rename failed --
// only a genuine write failure discards the tmp file.

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h> // _mkdir
#include <io.h>     // _commit, _fileno
#else
#include <sys/stat.h> // mkdir
#include <unistd.h>
int fileno(FILE* f);
int fsync(int fd);
#endif

#include "map_idcounts.h"
#include "../cNBT/list.h"
#include "../cNBT/nbt.h"

struct map_idcounts_entry {
	char name[MAP_IDCOUNTS_MAX_KEY_LEN + 1];
	int16_t value;
};

static void map_idcounts_make_dir(const char* path) {
#ifdef _WIN32
	_mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

static void map_idcounts_path(string_t out, string_t world_dir) {
	string_init_printf(out, "%s/data/idcounts.dat",
					   string_get_cstr(world_dir));
}

static bool map_idcounts_sync_file(FILE* f) {
	if(!f || fflush(f) != 0)
		return false;
#ifdef _WIN32
	return _commit(_fileno(f)) == 0;
#else
	return fsync(fileno(f)) == 0;
#endif
}

// Reads the WHOLE file into a malloc'd buffer. Returns NULL (with *out_len
// untouched) if the file cannot be opened, is empty, or a read fails --
// every one of those is treated identically to "no idcounts.dat yet" by the
// caller (Beta's own loadIdCounts wraps the whole load in a try/catch that
// leaves idCounts however far it got, never crashing the game over a bad
// counters file; starting this table empty on any anomaly is the same
// spirit of resilience).
static unsigned char* map_idcounts_read_whole_file(const char* path,
												   size_t* out_len) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return NULL;

	if(fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	long len = ftell(f);
	if(len < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}

	unsigned char* buf = malloc((size_t)len > 0 ? (size_t)len : 1);
	if(!buf) {
		fclose(f);
		return NULL;
	}

	size_t read = len > 0 ? fread(buf, 1, (size_t)len, f) : 0;
	fclose(f);
	if(read != (size_t)len) {
		free(buf);
		return NULL;
	}

	*out_len = (size_t)len;
	return buf;
}

// True iff `path` names a file that can be opened for reading right now --
// the same "does something exist here" probe map_data_archive.c's
// map_data_file_present uses to distinguish "nothing at path" (eligible for
// the "_new" crash-window fallback below) from "path exists but failed to
// parse" (present-but-corrupt, which must fail closed instead -- see
// map_idcounts_load_entries).
static bool map_idcounts_file_present(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	fclose(f);
	return true;
}

// Populates *entries/*count from a SPECIFIC on-disk file, if present and
// well-shaped (root TAG_COMPOUND, every direct child TAG_SHORT with a name
// short enough for MAP_IDCOUNTS_MAX_KEY_LEN, no more than
// MAP_IDCOUNTS_MAX_KEYS of them). Returns false (leaving *count at 0) on ANY
// shape anomaly, including the file being absent/unreadable -- matching
// Beta's own idCounts.clear()-then-best-effort-populate shape in
// loadIdCounts, and map_data_archive_load_path's identical contract.
static bool map_idcounts_load_entries_path(const char* path,
										   struct map_idcounts_entry* entries,
										   size_t* count) {
	*count = 0;

	size_t len = 0;
	unsigned char* buf = map_idcounts_read_whole_file(path, &len);
	if(!buf)
		return false;

	nbt_node* root = nbt_parse(buf, len);
	free(buf);
	if(!root)
		return false;

	if(root->type != TAG_COMPOUND) {
		nbt_free(root);
		return false;
	}

	bool overflowed = false;
	struct list_head* pos;
	list_for_each(pos, &root->payload.tag_compound->entry) {
		const nbt_node* child = list_entry(pos, struct nbt_list, entry)->data;
		if(!child || child->type != TAG_SHORT || !child->name
		   || strlen(child->name) > MAP_IDCOUNTS_MAX_KEY_LEN) {
			overflowed = true;
			break;
		}
		if(*count >= MAP_IDCOUNTS_MAX_KEYS) {
			overflowed = true;
			break;
		}
		strcpy(entries[*count].name, child->name);
		entries[*count].value = child->payload.tag_short;
		(*count)++;
	}

	nbt_free(root);

	if(overflowed) {
		*count = 0; // fail closed: treat like an absent/foreign file (see header)
		return false;
	}
	return true;
}

// Populates *entries/*count from idcounts.dat, with the same "_new"
// crash-window recovery map_data_archive_load implements (see this file's
// header comment): if the committed name is genuinely ABSENT (not merely
// unparsable), a save that completed its durable write+fsync but died
// between `remove(path)` and the promoting `rename()` in
// map_idcounts_write_entries leaves a complete, valid table sitting at
// "<path>_new" -- recover it instead of silently starting the table over
// from empty. A PRESENT-but-corrupt primary never falls back to "_new": that
// would risk resurrecting a stale, superseded table over a primary that
// merely bit-rotted for an unrelated reason (map_data_archive_load's own
// documented distinction). A successful "_new" recovery self-heals by
// promoting it onto the primary name so later loads take the fast path.
static void map_idcounts_load_entries(string_t world_dir,
									  struct map_idcounts_entry* entries,
									  size_t* count) {
	*count = 0;

	string_t path;
	map_idcounts_path(path, world_dir);

	bool primary_present = map_idcounts_file_present(string_get_cstr(path));
	bool ok = primary_present
		&& map_idcounts_load_entries_path(string_get_cstr(path), entries,
										   count);
	if(!ok && !primary_present) {
		string_t tmp_name;
		string_init_printf(tmp_name, "%s_new", string_get_cstr(path));
		ok = map_idcounts_load_entries_path(string_get_cstr(tmp_name), entries,
											count);
		if(ok) {
			// Best-effort -- a failed promote here just means the next load
			// repeats this same, still-correct, recovery (matches
			// map_data_archive_load's identical fallback).
			rename(string_get_cstr(tmp_name), string_get_cstr(path));
		}
		string_clear(tmp_name);
	}

	string_clear(path);
}

#ifdef CAVEWII_MAP_IDCOUNTS_TEST
// Forward-declared: needed by map_idcounts_write_entries, defined at EOF
// (mirrors map_data_archive.c's map_data_archive_test_should_fail
// placement/comment).
static bool map_idcounts_test_should_fail(
	enum map_idcounts_test_fail_stage stage);
#endif

// Durably writes `entries[0..count)` as the flat TAG_SHORT root compound
// [740c583: MapStorage.getUniqueDataId's write side]. Same temp+fsync+rename
// doctrine as map_data_archive_save, ONE generation (see this file's header
// comment for why no "_new" fallback).
static bool map_idcounts_write_entries(string_t world_dir,
									   const struct map_idcounts_entry* entries,
									   size_t count) {
	assert(count <= MAP_IDCOUNTS_MAX_KEYS);

	string_t dir;
	string_init_printf(dir, "%s/data", string_get_cstr(world_dir));
	map_idcounts_make_dir(string_get_cstr(dir));
	string_clear(dir);

	string_t path;
	map_idcounts_path(path, world_dir);

	// Hand-linked nbt_node/nbt_list array, same shape as
	// map_data_archive_save's `data_nodes`/`data_list` -- this tree must
	// NEVER be passed to nbt_free/nbt_free_list/nbt_clone (every name/data
	// pointer aliases `entries`/string literals, not owned heap memory).
	nbt_node nodes[MAP_IDCOUNTS_MAX_KEYS];
	struct nbt_list list_nodes[MAP_IDCOUNTS_MAX_KEYS];
	struct nbt_list sentinel = {.data = NULL};
	INIT_LIST_HEAD(&sentinel.entry);

	for(size_t k = 0; k < count; k++) {
		nodes[k] = (nbt_node) {
			.type = TAG_SHORT,
			.name = (char*)entries[k].name,
			.payload.tag_short = entries[k].value,
		};
		list_nodes[k].data = &nodes[k];
		list_add_tail(&list_nodes[k].entry, &sentinel.entry);
	}

	nbt_node root = {
		.type = TAG_COMPOUND,
		.name = "", // unnamed root compound, matching map_data_archive.c's
		.payload.tag_compound = &sentinel,
	};

	struct buffer raw = nbt_dump_binary(&root);
	if(!raw.data) {
		string_clear(path);
		return false;
	}

	string_t tmp_name;
	string_init_printf(tmp_name, "%s_new", string_get_cstr(path));

	bool write_ok = false;
	FILE* f = fopen(string_get_cstr(tmp_name), "wb");
	if(f) {
		write_ok = fwrite(raw.data, 1, raw.len, f) == raw.len;
		if(write_ok)
			write_ok = map_idcounts_sync_file(f);
		if(fclose(f) != 0)
			write_ok = false;
	}
#ifdef CAVEWII_MAP_IDCOUNTS_TEST
	if(map_idcounts_test_should_fail(MAP_IDCOUNTS_TEST_FAIL_WRITE))
		write_ok = false;
#endif
	buffer_free(&raw);

	bool ok = false;
	if(!write_ok) {
		// The write itself never became a complete, fsynced file -- whatever
		// is at tmp_name (if anything) is garbage/partial, not a recoverable
		// survivor. Discard it (map_data_archive_save's identical reasoning).
		remove(string_get_cstr(tmp_name));
	} else {
		// tmp_name now holds a complete, fsynced, valid write -- it is a real
		// survivor from this point on. If the promoting rename below fails
		// for ANY reason (a hard crash that skips the rest of this function,
		// or a transient AV/indexer/SD-I/O lock that lets this function keep
		// running), tmp_name must NOT be deleted: it is the only remaining
		// copy of the data just saved, and map_idcounts_load_entries's own
		// "_new" fallback exists precisely to recover it.
		remove(string_get_cstr(path));
#ifdef CAVEWII_MAP_IDCOUNTS_TEST
		if(map_idcounts_test_should_fail(MAP_IDCOUNTS_TEST_FAIL_RENAME))
			ok = false;
		else
#endif
			ok = rename(string_get_cstr(tmp_name), string_get_cstr(path)) == 0;
		// Success: tmp_name no longer exists (rename consumed it); `path`
		// holds the new data. Failure: tmp_name is DELIBERATELY left in
		// place -- do not remove it here.
	}

	string_clear(tmp_name);
	string_clear(path);
	return ok;
}

bool map_idcounts_get_unique_id(string_t world_dir, const char* key,
								int32_t* out_id) {
	assert(world_dir && key && out_id);
	// Inlined into the assert (rather than a named `key_len` local) so an
	// NDEBUG build that compiles asserts away never leaves an
	// unused-variable warning behind (CLAUDE.md's standing NDEBUG trap).
	assert(strlen(key) <= MAP_IDCOUNTS_MAX_KEY_LEN);

	struct map_idcounts_entry entries[MAP_IDCOUNTS_MAX_KEYS];
	size_t count = 0;
	map_idcounts_load_entries(world_dir, entries, &count);

	// [740c583: MapStorage.getUniqueDataId, MapStorage.java:134] reads the
	// per-key counter: a missing key starts at 0, otherwise the stored short
	// is incremented; the new value is stored back under the key.
	int found = -1;
	for(size_t k = 0; k < count; k++) {
		if(strcmp(entries[k].name, key) == 0) {
			found = (int)k;
			break;
		}
	}

	int16_t new_value;
	if(found < 0) {
		new_value = strcmp(key, "map") == 0 ? 1 : 0; // maintainer: ID 0 reserved
		// Append -- if the table is already full of OTHER keys this port
		// never writes, drop the oldest rather than refuse the caller a
		// fresh map id; this can only happen with a hand-edited/foreign
		// file, since this port and Beta 1.7.3 both only ever use "map".
		if(count >= MAP_IDCOUNTS_MAX_KEYS)
			count = MAP_IDCOUNTS_MAX_KEYS - 1;
		strncpy(entries[count].name, key, MAP_IDCOUNTS_MAX_KEY_LEN);
		entries[count].name[MAP_IDCOUNTS_MAX_KEY_LEN] = 0;
		entries[count].value = new_value;
		count++;
	} else {
		new_value = (int16_t)((int32_t)entries[found].value + 1);
		if(new_value == 0 && strcmp(key, "map") == 0) new_value = 1;
		entries[found].value = new_value;
	}

	// FIX (id-reuse-on-persistence-failure): this module keeps NO in-RAM
	// counter by design -- every call re-reads idcounts.dat from disk (see
	// this file's header comment). That means the ONLY thing that actually
	// "spends" `new_value` is the write below landing on disk. The previous
	// version of this function ignored map_idcounts_write_entries's return
	// value and handed `new_value` back regardless: on a failed write, the
	// on-disk table stays exactly as it was, so the NEXT call independently
	// re-derives this SAME `new_value` -- and if that next call is for a
	// DIFFERENT freshly-crafted map, two unrelated maps end up sharing one
	// id, hence one cache/archive record, each overwriting the other's
	// colour data. Propagate the failure instead: leave *out_id untouched
	// and tell the caller nothing was allocated. Still self-healing exactly
	// as this header documents -- since the table on disk is unchanged, the
	// next SUCCESSFUL call recomputes this identical id from scratch, so no
	// id is ever skipped either, only (correctly) retried.
	if(!map_idcounts_write_entries(world_dir, entries, count))
		return false;

	// Java short -> int widening on return (sign-extending) -- identical to
	// a plain C int16_t -> int32_t conversion.
	*out_id = (int32_t)new_value;
	return true;
}

#ifdef CAVEWII_MAP_IDCOUNTS_TEST
static enum map_idcounts_test_fail_stage map_idcounts_fail_stage
	= MAP_IDCOUNTS_TEST_FAIL_NONE;

void map_idcounts_test_fail_next(enum map_idcounts_test_fail_stage stage) {
	map_idcounts_fail_stage = stage;
}

static bool map_idcounts_test_should_fail(
	enum map_idcounts_test_fail_stage stage) {
	if(map_idcounts_fail_stage != stage)
		return false;
	map_idcounts_fail_stage = MAP_IDCOUNTS_TEST_FAIL_NONE;
	return true;
}
#endif
