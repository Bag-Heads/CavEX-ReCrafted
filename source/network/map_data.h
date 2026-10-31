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
	Beta MapData [740c583: MapData, MapDataBase] -- a saved map's persisted
	state, and the pure scan math from ItemMap.updateMapData. DATA LAYER ONLY
	(gap audit #10, mapdata-slice, part 1 of 2): this header owns the record
	shape, its defaults, and the scan loop's coordinate/gate/dimension/Nether-
	pick math. It deliberately excludes ItemMap itself (id allocation, the
	onCreated/onUpdate dispatch), the colour-palette block-histogram scan
	(the majority-block-id vote, water shading, brightness), and any client
	render -- see MAPDATA_SLICE_NOTES.md for those part-2 seams. Disk
	persistence lives in map_data_archive.h, kept separate so this header
	stays cNBT-free and trivially unit-testable.
*/

#ifndef NETWORK_MAP_DATA_H
#define NETWORK_MAP_DATA_H

#include <stdbool.h>
#include <stdint.h>

#include "../world.h" // enum world_dim

// Beta's map dimensions are hardcoded 128x128 shorts on every CaveWii-written
// file (see map_data_archive.h's read-side note on the mismatch branch this
// port does not reproduce), so they are compile-time constants here, not
// struct fields.
#define MAP_DATA_DIM 128
#define MAP_DATA_COLORS_SIZE (MAP_DATA_DIM * MAP_DATA_DIM) // 16384

struct map_data {
	int32_t x_center; // Beta xCenter
	int32_t z_center; // Beta zCenter
	int8_t dimension; // Beta dimension (byte); worldType: 0 overworld, -1 nether
	int8_t scale;     // Beta scale (byte); readFromNBT clamps this to 0..4
	uint8_t colors[MAP_DATA_COLORS_SIZE]; // Beta colors (byte[16384], 128x128)
	// Beta's own int field, but it is NEVER round-tripped through NBT --
	// MapData.writeToNBT/readFromNBT never touch it, so every load starts
	// back at Java's untouched field default (0), exactly as a freshly
	// `new MapData(name)` instance reads before readFromNBT runs.
	// [740c583: MapData, ItemMap.updateMapData]
	int32_t step;
};

// ---- ItemMap.getMapData's fallback defaults [740c583] ---------------------
// xCenter/zCenter/dimension are supplied by the caller so this single
// function backs BOTH of Beta's distinct default-populating call sites:
// getMapData's (world-spawn-centered) fallback --
//   xCenter = worldInfo.getSpawnX(); zCenter = worldInfo.getSpawnZ();
// -- and onCreated's (player-position-centered, fresh-item) path --
//   xCenter = MathHelper.floor_double(player.posX); zCenter = ...posZ);
// -- both otherwise identical (scale=3, dimension=(byte)worldProvider.
// worldType, markDirty). WHICH source to floor/pass in is an ItemMap-layer
// (part 2) decision -- see jmath.h's java_floor_double for the true-floor
// helper the onCreated flavor needs on its caller side.
void map_data_set_defaults(struct map_data* md, int32_t x_center,
						   int32_t z_center, int8_t dimension);

// ---- updateMapData's pure scan math [740c583: ItemMap.updateMapData] ------
// Deliberately excludes the colour-palette application (the per-pixel
// block-id histogram, water shading, brightness banding) -- that, and the
// caller loop that drives these in `x` then `z` order, is part 2's
// "ItemMap"/render scope.

// `1 << scale`. Beta: `int var6 = 1 << var3.scale;`. Precondition: scale is
// in the POST-CLAMP range readFromNBT/map_data_set_defaults guarantee, 0..4.
int32_t map_data_scan_blocks_per_pixel(int8_t scale);

// `128 / blocks_per_pixel`, halved in a hasNoSky dimension (the Nether).
// Beta: `int var11 = 128 / var6; if (var1.worldProvider.hasNoSky) { var11 /=
// 2; }`.
int32_t map_data_scan_radius(int32_t blocks_per_pixel, bool has_no_sky);

// The scanned pixel-space center for one axis: true-floors
// (entity_pos - map_center), integer-divides by blocks_per_pixel
// (truncating -- Java int/int on an already-integral floor result, which is
// identical to C's), then adds the fixed 64-pixel half-width. Beta:
// `MathHelper.floor_double(var2.posX - (double)var7) / var6 + var4 / 2`
// (var4 is the compile-time-constant 128). Call once per axis (X with
// xCenter, Z with zCenter) -- the two Beta lines are the same shape.
int32_t map_data_scan_pixel_center(double entity_pos, int32_t map_center,
								   int32_t blocks_per_pixel);

// The stripe gate: true iff pixel column `pixel_x` is scanned this call.
// Beta increments step FIRST, then gates every column with this test:
// `++var3.step; ... if ((var12 & 15) == (var3.step & 15))`. The caller owns
// the increment itself (a struct map_data field mutation, not this pure
// predicate) -- pass the step value AFTER incrementing.
bool map_data_scan_stripe_selected(int32_t pixel_x,
								   int32_t step_after_increment);

// The dimension guard: Beta only scans `if (worldProvider.worldType ==
// mapData.dimension) { ... }` -- a map created in/for another dimension
// never updates.
bool map_data_scan_should_update(enum world_dim world_dimension,
								 int8_t map_dimension);

// The Nether column pick [740c583: ItemMap.updateMapData, hasNoSky branch]:
// true = stone, false = dirt. All arithmetic is Java int32 wrap; see the .c
// for why the final `>> 20 & 1` is safe to compute with an unsigned
// (logical) shift despite Java's `>>` being arithmetic on a signed int.
bool map_data_scan_nether_column_is_stone(int32_t world_x, int32_t world_z);

#endif
