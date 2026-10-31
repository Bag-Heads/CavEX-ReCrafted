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

#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h> // _mkdir
#include <io.h>     // _commit, _fileno
#else
#include <sys/stat.h> // mkdir
#include <unistd.h>
// devkitPPC's newlib hides these POSIX declarations under -std=c99 -pedantic.
int fileno(FILE* f);
int fsync(int fd);
#endif

#include "map_data_archive.h"
#include "../cNBT/nbt.h"

// mkdir helper (mirrors region_archive.c's region_make_dir): EEXIST is fine,
// any REAL problem (e.g. an unwritable world_dir) surfaces at the fopen that
// follows.
static void map_data_make_dir(const char* path) {
#ifdef _WIN32
	_mkdir(path);
#else
	mkdir(path, 0777);
#endif
}

static void map_data_archive_path(string_t out, string_t world_dir,
								  int32_t id) {
	string_init_printf(out, "%s/data/map_%d.dat", string_get_cstr(world_dir),
					   id);
}

// fflush then platform durability barrier (fsync/_commit). Mirrors
// level_archive.c/death_drop_journal.c's identical helper: fflush alone only
// reaches libfat's cache on the Wii, not the card.
static bool map_data_sync_file(FILE* f) {
	if(!f || fflush(f) != 0)
		return false;
#ifdef _WIN32
	return _commit(_fileno(f)) == 0;
#else
	return fsync(fileno(f)) == 0;
#endif
}

// Does a file exist at `path` at all, independent of whether its contents
// parse or validate? Used to distinguish "nothing at path" (the ONLY case
// map_data_archive_load's "_new" fallback exists for) from "path exists but
// is corrupt/wrong-shape" (which must fail closed instead -- see that
// function). A plain fopen probe rather than reading nbt_parse_path's errno:
// explicit and self-contained rather than coupled to cNBT's internal error
// code convention.
static bool map_data_file_present(const char* path) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	fclose(f);
	return true;
}

#ifdef CAVEWII_MAP_DATA_ARCHIVE_TEST
// Forward-declared: needed by map_data_archive_save, defined at EOF (mirrors
// region_archive.c's region_archive_test_should_fail placement/comment).
static bool map_data_archive_test_should_fail(
	enum map_data_archive_test_fail_stage stage);
#endif

bool map_data_archive_save(string_t world_dir, int32_t id,
						   const struct map_data* md) {
	assert(world_dir && md && id >= INT16_MIN && id <= INT16_MAX);

	string_t dir;
	string_init_printf(dir, "%s/data", string_get_cstr(world_dir));
	map_data_make_dir(string_get_cstr(dir));
	string_clear(dir);

	string_t path;
	map_data_archive_path(path, world_dir, id);

	// ---- Build the dump-only tree: root ("") -> "data" -> the 7 Beta tags
	// [740c583: MapStorage.saveData wraps writeToNBT's compound in a second,
	// "data"-named compound under an unnamed root; MapData.writeToNBT emits
	// dimension/xCenter/zCenter/scale/width/height/colors, in that order].
	// Every node below is stack/compound-literal memory, and the colors
	// node points straight at md's own buffer with NO copy -- this tree must
	// NEVER be passed to nbt_free/nbt_free_list/nbt_clone (region_archive.c's
	// chunk writer is the precedent for this exact shape and warning).
	struct nbt_list data_list_sentinel = (struct nbt_list) {.data = NULL};
	INIT_LIST_HEAD(&data_list_sentinel.entry);

	nbt_node data_nodes[] = {
		{.type = TAG_BYTE,
		 .name = "dimension",
		 .payload.tag_byte = md->dimension},
		{.type = TAG_INT, .name = "xCenter", .payload.tag_int = md->x_center},
		{.type = TAG_INT, .name = "zCenter", .payload.tag_int = md->z_center},
		{.type = TAG_BYTE, .name = "scale", .payload.tag_byte = md->scale},
		{.type = TAG_SHORT, .name = "width", .payload.tag_short = MAP_DATA_DIM},
		{.type = TAG_SHORT,
		 .name = "height",
		 .payload.tag_short = MAP_DATA_DIM},
		{.type = TAG_BYTE_ARRAY,
		 .name = "colors",
		 .payload.tag_byte_array.data = (unsigned char*)md->colors,
		 .payload.tag_byte_array.length = MAP_DATA_COLORS_SIZE},
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

	// ---- Durable write: tmp file, fsync, atomic rename over the committed
	// name (level_archive.c/death_drop_journal.c's doctrine, sized to one
	// generation -- see this function's header comment for why no `_old`
	// backup is kept). `remove(path)` before the rename is required for
	// Windows rename() semantics (unlike POSIX, it fails if the destination
	// already exists); it is the same shape level_archive_flush uses to
	// guarantee the FINAL rename's destination is absent.
	string_t tmp_name;
	string_init_printf(tmp_name, "%s_new", string_get_cstr(path));

	bool write_ok = false;
	FILE* f = fopen(string_get_cstr(tmp_name), "wb");
	if(f) {
		write_ok = nbt_dump_file(&root, f, STRAT_GZIP) == NBT_OK;
		if(write_ok)
			write_ok = map_data_sync_file(f);
		if(fclose(f) != 0) // flush/close failure (e.g. disk full) surfaces here
			write_ok = false;
	}

	bool ok = false;
	if(!write_ok) {
		// The write itself never became a complete, fsynced file -- whatever
		// is at tmp_name (if anything) is garbage/partial, not a recoverable
		// survivor. Discard it; `path` (if it holds a previously committed
		// file) has not been touched at all yet, so this failure loses
		// nothing.
		remove(string_get_cstr(tmp_name));
	} else {
		// tmp_name now holds a complete, fsynced, valid write -- it is a
		// real survivor from this point on. If the promoting rename below
		// fails for ANY reason (not only a hard crash that skips this whole
		// function), tmp_name must NOT be deleted: it is the only remaining
		// copy of the data just saved, and map_data_archive_load's own
		// "_new" fallback exists precisely to recover it (see that function
		// and docs/deviations.md's "one durable-write generation" entry).
		// The previous version of this code deleted tmp_name unconditionally
		// whenever `ok` was false at the end -- which, on a non-fatal rename
		// failure (e.g. a transient AV/indexer lock on Windows, a transient
		// SD I/O error on Wii), destroyed the fsynced survivor the fallback
		// was supposed to find, losing BOTH the old and the new copy.
		remove(string_get_cstr(path));
#ifdef CAVEWII_MAP_DATA_ARCHIVE_TEST
		if(map_data_archive_test_should_fail(MAP_DATA_ARCHIVE_TEST_FAIL_RENAME))
			ok = false;
		else
#endif
			ok = rename(string_get_cstr(tmp_name), string_get_cstr(path)) == 0;
		// Success: tmp_name no longer exists (rename consumed it); `path`
		// holds the new data. Failure: tmp_name is DELIBERATELY left in
		// place below -- do not remove it.
	}

	string_clear(tmp_name);
	string_clear(path);
	return ok;
}

// Parse a single map_data record out of an already-parsed tree. Commits
// nothing to *out until every field has validated (the level_archive.c
// "commit last" convention) -- a partial/malformed tree leaves *out
// untouched.
static bool map_data_from_tree(nbt_node* root, struct map_data* out) {
	assert(root && out);

	nbt_node* dim = nbt_find_by_path(root, ".data.dimension");
	nbt_node* xc = nbt_find_by_path(root, ".data.xCenter");
	nbt_node* zc = nbt_find_by_path(root, ".data.zCenter");
	nbt_node* sc = nbt_find_by_path(root, ".data.scale");
	nbt_node* w = nbt_find_by_path(root, ".data.width");
	nbt_node* h = nbt_find_by_path(root, ".data.height");
	nbt_node* colors = nbt_find_by_path(root, ".data.colors");

	if(!dim || dim->type != TAG_BYTE || !xc || xc->type != TAG_INT || !zc
	   || zc->type != TAG_INT || !sc || sc->type != TAG_BYTE || !w
	   || w->type != TAG_SHORT || !h || h->type != TAG_SHORT || !colors
	   || colors->type != TAG_BYTE_ARRAY)
		return false;

	// [740c583: MapData.readFromNBT] The fast (`width == 128 && height ==
	// 128`) path is the only one this port implements -- see
	// map_data_archive.h's doc comment on the mismatch branch it
	// deliberately does not reproduce. CaveWii never writes anything but
	// MAP_DATA_DIM/MAP_DATA_DIM, so a mismatch here means a foreign/hand-
	// edited/corrupt file; fail closed rather than guess.
	if(w->payload.tag_short != MAP_DATA_DIM
	   || h->payload.tag_short != MAP_DATA_DIM)
		return false;
	// The fast path's `this.colors = var1.getByteArray("colors");` trusts
	// the array's length in Beta (a bounds-checked language); our fixed
	// 16384-byte C destination cannot, so this length check is load-bearing
	// (not merely defensive) against a memcpy over-read from a truncated
	// "colors" tag.
	if(colors->payload.tag_byte_array.length != MAP_DATA_COLORS_SIZE)
		return false;

	int8_t scale = sc->payload.tag_byte;
	// [740c583: MapData.readFromNBT] `if (this.scale < 0) { this.scale = 0;
	// } if (this.scale > 4) { this.scale = 4; }`
	if(scale < 0)
		scale = 0;
	if(scale > 4)
		scale = 4;

	out->dimension = dim->payload.tag_byte;
	out->x_center = xc->payload.tag_int;
	out->z_center = zc->payload.tag_int;
	out->scale = scale;
	memcpy(out->colors, colors->payload.tag_byte_array.data,
		  MAP_DATA_COLORS_SIZE);
	// step is never persisted [740c583: MapData -- readFromNBT never
	// assigns it]; every load starts at Java's untouched field default.
	out->step = 0;
	return true;
}

static bool map_data_archive_load_path(const char* path, struct map_data* out) {
	nbt_node* root = nbt_parse_path(path);
	if(!root)
		return false;
	bool ok = map_data_from_tree(root, out);
	nbt_free(root);
	return ok;
}

bool map_data_archive_load(string_t world_dir, int32_t id,
						   struct map_data* out) {
	assert(world_dir && out && id >= INT16_MIN && id <= INT16_MAX);

	string_t path;
	map_data_archive_path(path, world_dir, id);

	// Distinguish "nothing at path" from "path exists but is corrupt/wrong-
	// shape" BEFORE attempting the parse: the "_new" fallback below may only
	// fire on the former. A present-but-unusable primary must fail closed
	// instead of silently substituting a possibly much older "_new" survivor
	// -- a single bool from the parse alone cannot make that distinction (a
	// corrupt primary and an absent one both parse to `false`).
	bool primary_present = map_data_file_present(string_get_cstr(path));
	bool ok = primary_present
		&& map_data_archive_load_path(string_get_cstr(path), out);
	if(!ok && !primary_present) {
		// Crash-window recovery: a save that completed its write + fsync but
		// died between `remove(path)` and the promoting rename (above)
		// leaves a complete, valid write sitting at "<path>_new" and
		// nothing at `path`. This is the same survivor tier
		// level_archive_create checks (its own "_new" fallback); covered
		// here without a second ("_old") generation, per
		// map_data_archive_save's documented one-generation tradeoff.
		string_t tmp_name;
		string_init_printf(tmp_name, "%s_new", string_get_cstr(path));
		ok = map_data_archive_load_path(string_get_cstr(tmp_name), out);
		if(ok) {
			// Self-heal: promote the recovered survivor onto the primary
			// name so subsequent loads take the fast (primary) path instead
			// of re-parsing "_new" every time, and so it does not linger
			// indefinitely, silently reusable against some later, unrelated
			// corruption of `path`. Best-effort -- a failed rename here just
			// means the next load repeats this same, still-correct,
			// recovery.
			rename(string_get_cstr(tmp_name), string_get_cstr(path));
		}
		string_clear(tmp_name);
	}

	string_clear(path);
	return ok;
}

void map_data_load_or_create(string_t world_dir, int32_t id, int32_t spawn_x,
							 int32_t spawn_z, int8_t dimension,
							 struct map_data* out) {
	// CODESTYLE 1.6's house-style compound precondition assert
	// (`assert(w && dimension >= -1 && dimension <= 0);`, server_world.c:202)
	// applied to `dimension` here too: this is the pass-through a future
	// part-2 (ItemMap) caller reaches FIRST, ahead of map_data_set_defaults
	// itself.
	assert(world_dir && out && id >= INT16_MIN && id <= INT16_MAX && dimension >= -1 && dimension <= 0);
	if(!map_data_archive_load(world_dir, id, out))
		map_data_set_defaults(out, spawn_x, spawn_z, dimension);
}

#ifdef CAVEWII_MAP_DATA_ARCHIVE_TEST
static enum map_data_archive_test_fail_stage map_data_archive_fail_stage
	= MAP_DATA_ARCHIVE_TEST_FAIL_NONE;

void map_data_archive_test_fail_next(
					   enum map_data_archive_test_fail_stage stage) {
	map_data_archive_fail_stage = stage;
}

static bool map_data_archive_test_should_fail(
						 enum map_data_archive_test_fail_stage stage) {
	if(map_data_archive_fail_stage != stage)
		return false;
	map_data_archive_fail_stage = MAP_DATA_ARCHIVE_TEST_FAIL_NONE;
	return true;
}
#endif
