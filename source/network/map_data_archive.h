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

/*
	Per-map save file: "<world_dir>/data/map_<id>.dat"
	[740c583: SaveHandler ctor's "data" subdir + SaveHandler.getMapFile]. `id`
	is the item's DAMAGE value (the map number) -- Beta's storage id string is
	`"map_" + itemDamage` [740c583: ItemMap.getMapData]. Allocating a FRESH id
	(MapStorage.getUniqueDataId / the "idcounts" file) is an ItemMap-layer
	concern, out of scope here (part 2): this module only persists/loads the
	record for an id it is handed.
*/

#ifndef NETWORK_MAP_DATA_ARCHIVE_H
#define NETWORK_MAP_DATA_ARCHIVE_H

#include <m-lib/m-string.h>
#include <stdbool.h>
#include <stdint.h>

#include "map_data.h"

// Save `md` to "<world_dir>/data/map_<id>.dat" as a gzip-compressed NBT tree,
// wrapped in MapStorage.saveData's un-named-root -> "data" compound
// [740c583: MapStorage.saveData, MapData.writeToNBT]. Creates
// "<world_dir>/data/" on demand if missing (this data-layer slice has no
// world-open hook to mirror SaveHandler's ctor-time mkdirs() at; see
// MAPDATA_SLICE_NOTES.md). Writes via a temp file + fsync + atomic rename
// over the previously committed file (the level_archive.c/
// death_drop_journal.c durability doctrine, sized down to ONE generation --
// no `_old` retention: a map's colours are regenerable gameplay content, not
// the only copy of the world). Returns false on any IO failure.
//
// A failure during the WRITE phase (temp-file open/dump/fsync/close) leaves
// the previously committed file, if any, completely untouched -- nothing
// durable was ever produced. A failure during the PROMOTING RENAME (after
// the write phase already completed and fsynced) is different: the
// previously committed file has, by then, already been removed (required
// for Windows rename() semantics -- see the .c), so it is genuinely gone;
// but the just-written, fsynced replacement is DELIBERATELY left sitting at
// "<path>_new" rather than deleted, because map_data_archive_load's own
// "_new" fallback exists precisely to recover it. Never treat a `false`
// return as "the map reverted to its previous state" -- check
// map_data_archive_load (or docs/deviations.md's "one durable-write
// generation" entry) for what is actually recoverable after a failed save.
bool map_data_archive_save(string_t world_dir, int32_t id,
						   const struct map_data* md);

// Parses "<world_dir>/data/map_<id>.dat" into *out. Returns false, leaving
// *out untouched, if the tree fails any shape/type check -- including a
// non-128x128 width/height, which this port deliberately does NOT attempt to
// resize/recenter. [740c583: MapData.readFromNBT] has a branch for that
// mismatch, but its bounds guard is `var8 >= 0 || var8 < 128` -- an `||`
// where an `&&` was surely meant, making the condition ALWAYS true. In Java
// that merely lets a malformed file throw ArrayIndexOutOfBoundsException;
// ported verbatim into C's fixed 16384-byte `colors` extent it would be a
// real out-of-bounds write. The branch is unreachable in vanilla Beta anyway
// (MapData.writeToNBT always emits width=height=128), so failing the load
// closed instead is a documented, zero-cost substitution, not a
// faithfulness loss. Applies the scale<0/>4 clamp [740c583:
// MapData.readFromNBT] to whatever scale IS accepted before returning.
//
// The "_new" crash-recovery fallback is consulted ONLY when the primary
// file is genuinely ABSENT (no bytes at "<path>" at all) -- never merely
// because it failed to parse or validate. A PRESENT-but-corrupt/wrong-shape
// primary (e.g. real on-disk bit-rot, unrelated to any save crash) fails
// closed instead of silently substituting a possibly much older "_new"
// survivor left over from some earlier interrupted save -- the same
// "a rejected/unusable tree must not silently roll state backward onto an
// older backup" rule level_archive_create applies to its own _new/_old
// tiers. A successful fallback recovery self-heals by promoting "_new" onto
// the primary name (best-effort; a failed promote just means the next load
// repeats the same, still-correct, recovery) so it does not linger and get
// silently reused indefinitely.
bool map_data_archive_load(string_t world_dir, int32_t id, struct map_data* out);

// Loads id's file, or -- if absent/malformed -- populates *out with
// map_data_set_defaults(spawn_x, spawn_z, dimension), the ItemMap.getMapData
// fallback shape. Pure composition of the two calls above: it does not
// allocate a fresh id, decide dirty/save timing, or cache resident records
// across calls. A resident id-keyed cache (Beta's own MapStorage.
// loadedDataMap; this dispatch's tile_entities_store.c-style "per-object
// store" file-map pointer) needs a real owning server lifetime to hang
// eviction/dirty-tracking off of, which this data-layer slice does not have
// -- left as the part-2 (ItemMap) seam. See MAPDATA_SLICE_NOTES.md.
void map_data_load_or_create(string_t world_dir, int32_t id, int32_t spawn_x,
							 int32_t spawn_z, int8_t dimension,
							 struct map_data* out);

#ifdef CAVEWII_MAP_DATA_ARCHIVE_TEST
// Test-only fault injection (mirrors region_archive.c's single-stage
// `_test_fail_next` shape): forces the NEXT occurrence of `stage` inside
// map_data_archive_save to fail, then self-resets to NONE. Lets a test
// deterministically exercise the promoting-rename failure path (a real OS
// race otherwise) so the "_new" survivor is provably not deleted rather than
// merely believed not to be.
enum map_data_archive_test_fail_stage {
	MAP_DATA_ARCHIVE_TEST_FAIL_NONE = 0,
	MAP_DATA_ARCHIVE_TEST_FAIL_RENAME,
};
void map_data_archive_test_fail_next(
	enum map_data_archive_test_fail_stage stage);
#endif

#endif
