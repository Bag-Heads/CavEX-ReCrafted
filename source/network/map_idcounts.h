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
	Beta MapStorage.getUniqueDataId [740c583: MapStorage.getUniqueDataId,
	MapStorage.loadIdCounts]: the persisted allocation counter behind
	World.getUniqueDataId, keyed by an arbitrary string ("map" is the ONLY key
	either Beta 1.7.3 or this port ever uses -- ItemMap.getMapData/
	getMPMapData/onCreated all pass the literal "map"). File:
	"<world_dir>/data/idcounts.dat" [740c583: SaveHandler.getMapFile("idcounts")
	-- same "data/" directory and "<name>.dat" naming as a map_N.dat file].

	DECOMPILE SURPRISE, flagged per this project's "decompile wins over any
	paraphrase" rule: unlike every map_N.dat (gzip-NBT via
	CompressedStreamTools.writeGzippedCompoundToOutputStream /
	loadGzippedCompoundFromOutputStream, see map_data_archive.c),
	idcounts.dat is RAW (uncompressed) NBT.
	MapStorage.loadIdCounts opens a plain DataInputStream directly over the
	file and calls CompressedStreamTools.read(DataInputStream) -- the PLAIN
	overload, not the "Gzipped" one -- and MapStorage.getUniqueDataId writes
	back via the equally plain CompressedStreamTools.write(NBTTagCompound,
	DataOutput). This module follows the decompile, not the "existing
	map_data_archive gzip-NBT style" phrasing in the part-2a brief.

	Root shape: an UNNAMED TAG_COMPOUND of TAG_SHORT entries, one per
	ever-used key [740c583: `NBTTagCompound var4 = new NBTTagCompound();
	... var4.setShort(var6, var7);` per key in idCounts, written on EVERY
	call]. This port only ever writes/reads the "map" key (see above), but
	preserves up to MAP_IDCOUNTS_MAX_KEYS other TAG_SHORT entries a foreign/
	real-Beta-written file might carry, so importing such a save does not
	silently drop unrelated counters.
*/

#ifndef NETWORK_MAP_IDCOUNTS_H
#define NETWORK_MAP_IDCOUNTS_H

#include <m-lib/m-string.h>
#include <stdbool.h>
#include <stdint.h>

// Generous headroom over the ONE key ("map") either Beta 1.7.3 or this port
// ever writes; see the file header. A file that already carries more than
// this many distinct TAG_SHORT keys is treated the same as a missing/corrupt
// one (see the .c) -- fail closed rather than silently truncate someone
// else's counters.
#define MAP_IDCOUNTS_MAX_KEYS 8
// Beta never emits a key longer than "map" (3 chars); this is a generous
// defensive bound on a KEY NAME's length, not a Beta-derived constant.
#define MAP_IDCOUNTS_MAX_KEY_LEN 15

// Maintainer chest-sort ruling: the map key reserves zero (first ID 1,
// also skips zero on wrap); other keys retain Beta arithmetic.
// Beta World.getUniqueDataId(key) -> MapStorage.getUniqueDataId(key)
// [740c583]: computes the NEXT id for `key` (0 on the first-ever call for a
// key never seen before, else the stored count + 1 -- Java: `Short.valueOf(
// (short)0)` vs `(short)(var2 + 1)`) and DURABLY persists the whole updated
// table before returning, synchronously, every single call -- Beta's
// getUniqueDataId has no dirty-flag/batched-flush semantics of its own
// (unlike MapData's markDirty()).
//
// The stored counter is a Java `short` (int16), so this replicates Java's
// short arithmetic (wraps at +-32768) before widening back to the returned
// int32_t, exactly mirroring short->int implicit widening on the Java side;
// wraparound is unreachable in realistic play (32768 maps crafted in one
// world) and is not specially handled beyond this faithful arithmetic.
//
// Returns true and writes the newly-reserved id to *out_id iff the updated
// table was actually committed to idcounts.dat. Returns false (leaving
// *out_id untouched) if the durable write failed -- this module keeps NO
// in-RAM counter (every call re-reads idcounts.dat from disk, see the .c's
// header comment), so a caller that used the in-memory `new_value` anyway on
// a failed write would hand out an id the table on disk never actually
// reserved: the very next call would recompute that SAME id again (since the
// on-disk table is unchanged) and could hand it to a second, unrelated
// caller -- two maps silently sharing one cache/archive record, each
// overwriting the other's data. Callers MUST treat a false return as "no id
// was allocated" and must not create/persist anything keyed by *out_id in
// that case. This is still self-healing: because the on-disk table is
// unchanged, the next SUCCESSFUL call recomputes the identical next id from
// scratch, so no id is ever skipped either.
//
// world_dir: the world folder (same string this engine already threads as
// `world->level_name` into region/map_data_archive paths). key: a
// NUL-terminated C string, <= MAP_IDCOUNTS_MAX_KEY_LEN chars (asserted).
bool map_idcounts_get_unique_id(string_t world_dir, const char* key,
								int32_t* out_id);

#ifdef CAVEWII_MAP_IDCOUNTS_TEST
// Test-only fault injection (mirrors region_archive.c's/map_data_archive.c's
// single-stage `_test_fail_next` shape -- CODESTYLE's runtime-armed-seam
// rule: a function that's always compiled in but inert unless a test arms
// it, never a second compile profile). Forces the NEXT
// map_idcounts_write_entries call inside map_idcounts_get_unique_id to
// report failure (as if the real fopen/fwrite/fsync/rename sequence had
// failed), then self-resets to NONE. Lets a test deterministically exercise
// the "durable write failed -> no id handed out, nothing skipped or reused"
// contract documented above without depending on an OS-specific way to make
// a real write actually fail.
enum map_idcounts_test_fail_stage {
	MAP_IDCOUNTS_TEST_FAIL_NONE = 0,
	MAP_IDCOUNTS_TEST_FAIL_WRITE,
	// Forces the promoting rename() inside map_idcounts_write_entries to
	// report failure AFTER a successful write+fsync+remove(path) -- exercises
	// the "_new" crash-window survivor path (map_idcounts_load_entries)
	// deterministically, mirroring
	// MAP_DATA_ARCHIVE_TEST_FAIL_RENAME.
	MAP_IDCOUNTS_TEST_FAIL_RENAME,
};
void map_idcounts_test_fail_next(enum map_idcounts_test_fail_stage stage);
#endif

#endif
