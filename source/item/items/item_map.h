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
	Beta 1.7.3 map item, part 2a (simulation): ItemMap onCreated/onUpdate
	dispatch, the block -> MapColor colour-scan core, and the resident
	map-data cache. Part 1 (source/network/map_data.h/.c,
	map_data_archive.h/.c) owns the persisted record shape, the gzip-NBT
	save file, and updateMapData's pure geometry/gate helpers -- this module
	is the "ItemMap" layer those files' own doc comments left as a seam.
	Held-map terrain and player icons render through graphics/render_item.c.
*/

#ifndef ITEM_ITEMS_ITEM_MAP_H
#define ITEM_ITEMS_ITEM_MAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../world.h" // enum world_dim
#include "../item_types.h"     // struct item

struct server_local;
struct map_data;
struct server_world;

/* Packet131's unsigned-byte length admits at most (255 - 1) / 3 icons. */
#define ITEM_MAP_MAX_MARKERS 84
struct item_map_marker {
	int8_t type, x, y, rotation;
};
/* CFR server MapData.java:95-110. False means hidden (dead, dimension
 * mismatch, or outside inclusive +/-64 pixels); on-map edges wrap as bytes. */
bool item_map_player_marker(const struct map_data* md, double x, double z,
	float yaw, int8_t dimension, bool alive, struct item_map_marker* out);
/* Jad client MapData.java:147-158. out owns ITEM_MAP_MAX_MARKERS entries.
 * Decode signed bytes; replace the list, ignoring trailing partial triples. */
size_t item_map_decode_markers(const uint8_t* bytes, uint8_t length,
	struct item_map_marker* out);

extern struct item item_map;

// [740c583: Block.blocksList[id].blockMaterial.materialMapColor.colorIndex]
// Per-BLOCK-ID colour-palette index (0..13), see item_map.c for the full
// hand-derived table + citations. Air (id 0) and every unregistered id
// (Beta 1.7.3 tops out at 96; this port also carries the NET-NEW relocated
// BLOCK_IRON_CHEST=200) return 0 (MapColor.airColor's own index).
uint8_t item_map_color_index_for_block(uint16_t block_id);

// [740c583: MapColor.java] The 14 MapColor colorValue RGBs, packed
// 0xRRGGBB, indexed by colorIndex (0..13) -- encoded here for the 2b render
// to consume later (see docs/deviations.md's render-deferral entry).
uint32_t item_map_color_rgb_for_index(uint8_t color_index);

// [740c583: MapItemRenderer.renderMap] Part 2b's pure-math half: per-pixel
// colour-byte -> RGBA8888 display conversion for a held/in-hand map's
// 128x128 colour buffer. `colors` is MAP_DATA_COLORS_SIZE (16384) bytes
// (struct map_data's own colors[] field, network/map_data.h); `out_rgba` is
// 4x that (65536 bytes, RGBA8888, row-major matching `colors`' own layout).
// Transcribes renderMap's intArray[] fill loop EXACTLY -- the unexplored
// checkerboard dither (alpha only, RGB stays black) and the palette-lookup +
// per-band (180/220/220/255) brightness multiply for an explored pixel; see
// item_map_render_convert.c for the full citation. Pure function: no I/O, no
// globals. Texture upload, terrain and player-icon drawing live in
// graphics/render_item.c; the map-name label remains deferred.
void item_map_colors_to_rgba(const uint8_t* colors, uint8_t* out_rgba);

// [740c583: ItemMap.updateMapData] Full transcription of the colour-scan
// loop (histogram/majority-vote, water-depth shading, brightness banding,
// edge/checkerboard dither), built on top of map_data.c's pure geometry/
// gate/dimension/Nether-pick helpers. Mutates `md` in place (colors[],
// step). Returns true iff at least one pixel's stored byte actually changed
// this call (Beta `var13 <= var14` -> MapData.setColumnDirty; the ONLY
// observable effect of setColumnDirty this SP-only port needs is "mark this
// map dirty for the next flush" -- its per-column min/max tracking exists
// in Beta solely to drive MULTIPLAYER incremental sync, out of scope here
// independently of the transient player-icon transport).
bool item_map_update_data(struct server_local* s, struct map_data* md,
						  double entity_x, double entity_z,
						  enum world_dim world_dim);
// Same scan/order, reading the supplied world's terrain and dimension.
bool item_map_update_data_w(struct server_world* w, struct map_data* md,
							double entity_x, double entity_z);

// Resident map-data cache (the "part 2 ItemMap seam" MAPDATA_SLICE_NOTES.md
// flagged; see server_local.h's MAX_MAP_CACHE doc comment).
//
// item_map_cache_resident: returns a pointer to `id`'s ALREADY-resident
// entry (touching its LRU stamp), or NULL if `id` is not cached -- the
// caller then decides how to populate it (load from disk, or allocate a
// fresh id) and calls item_map_cache_install. Mirrors Beta's MapStorage.
// loadedDataMap cache-hit check (`if (var3 != null) return var3;`) staying
// a SEPARATE step from the disk-load/allocate fallback in both
// ItemMap.getMPMapData and ItemMap.getMapData.
struct map_data* item_map_cache_resident(struct server_local* s, int32_t id);
// Installs `*md` into the cache under `id` (evicting the LRU resident slot,
// flushing it first if dirty, when the cache is full), marked dirty
// per `dirty`. Used both for a disk-loaded entry (dirty=false, matching
// Beta's MapStorage.loadData never marking a freshly loaded MapData dirty)
// and a freshly-created one (dirty=true, matching markDirty() in both
// ItemMap.onCreated and ItemMap.getMapData's fallback branch). Returns false
// if saving a dirty eviction victim fails; the victim remains resident.
bool item_map_cache_install(struct server_local* s, int32_t id,
							const struct map_data* md, bool dirty);
// Marks the cache entry for `id` dirty (must already be resident).
void item_map_cache_mark_dirty(struct server_local* s, int32_t id);
// Flushes every dirty resident cache entry to disk (map_data_archive_save)
// and clears their dirty flags. Called from the same checkpoint/autosave
// boundaries chests/signs/furnaces already use (server_local.c) -- see
// server_local.h's MAX_MAP_CACHE doc comment for why maps need their OWN
// flush call instead of piggybacking the chunk-dirty-marking trick those
// chunk-bound tile entities use.
void item_map_cache_flush_all(struct server_local* s);

#endif
