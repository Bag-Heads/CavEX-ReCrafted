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

// Beta 1.7.3 ItemMap [740c583: ItemMap.java, ItemMapBase.java]. See
// item_map.h for the module's scope and its relationship to part 1
// (map_data.h/.c, map_data_archive.h/.c).

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "item_map.h"

#include "../../block/blocks_data.h"
#include "../../graphics/render_item.h"
#include "../../network/map_data_archive.h"
#include "../../network/map_idcounts.h"
#include "../../network/client_interface.h"
#include "../../network/server_local.h"
#include "../../network/server_local_state.h" // full server state (split from server_local.h)
#include "../../network/server_world.h"
#include "../../worldgen/jmath.h" // java_floor_double

bool item_map_player_marker(const struct map_data* md, double x, double z,
	float yaw, int8_t dimension, bool alive, struct item_map_marker* out) {
	/* CFR server MapData.java:95-110; Jad client MapData.java:89-109.
	 * The float cast precedes division; +0.5 then truncation is asymmetric.
	 * At +64 the resulting 128 wraps as a Java byte. Off-map icons vanish. */
	assert(md && out && md->scale >= 0 && md->scale <= 4);
	if(!alive || dimension != md->dimension || !isfinite(yaw)) return false;
	float px = (float)(x - (double)md->x_center) / (float)(1 << md->scale);
	float py = (float)(z - (double)md->z_center) / (float)(1 << md->scale);
	if(!(px >= -64 && px <= 64 && py >= -64 && py <= 64)) return false;
	int32_t rotation = java_double_to_int((double)(yaw * 16.0F / 360.0F) + 0.5);
	if(md->dimension < 0) {
		uint32_t step = (uint32_t)(md->step / 10);
		rotation = (int32_t)((step * step * 34187121u + step * 121u) >> 15 & 15u);
	}
	*out = (struct item_map_marker) {
		.type = 0,
		.x = (int8_t)(uint8_t)(int32_t)((double)(px * 2.0F) + 0.5),
		.y = (int8_t)(uint8_t)(int32_t)((double)(py * 2.0F) + 0.5),
		.rotation = (int8_t)(uint8_t)rotation,
	};
	return true;
}

size_t item_map_decode_markers(const uint8_t* bytes, uint8_t length,
	struct item_map_marker* out) {
	/* MapData.java:147-158: SIGNED Java byte %16 and /16, not nibble masks.
	 * Incomplete trailing triples are ignored by (length - 1) / 3. */
	if(!bytes || !length || bytes[0] != 1 || !out) return 0;
	size_t count = (length - 1) / 3;
	for(size_t i = 0; i < count; i++) {
		int packed = (int8_t)bytes[i * 3 + 1];
		out[i] = (struct item_map_marker) {
			.type = packed % 16, .rotation = packed / 16,
			.x = (int8_t)bytes[i * 3 + 2], .y = (int8_t)bytes[i * 3 + 3],
		};
	}
	return count;
}

// ============================================================================
// Block -> MapColor colour-index table [740c583]
// ============================================================================
// Beta: Block.blocksList[id].blockMaterial.materialMapColor.colorIndex.
// Hand-derived by chasing THREE decompile files for every one of Beta
// 1.7.3's 97 registered block ids (0..96):
//   1) Block.java's static initializer (the BlockXxx constructor call,
//      or occasionally an explicit `Material.xxx` literal passed straight
//      in, e.g. Block(4, 16, Material.rock) for cobblestone);
//   2) that block's own subclass constructor (fetched individually --
//      BlockStone, BlockGrass, BlockSand, ... -- via
//      `beta.sh <ClassName>`), which either passes its OWN literal
//      `Material.xxx` to `super(...)`, or (for a handful of `BlockXxx
//      extends BlockYyy` subclasses with a trivial pass-through
//      constructor, e.g. BlockSapling extends BlockFlower, BlockGravel
//      extends BlockSand, BlockObsidian extends BlockStone,
//      BlockDetectorRail/BlockTallGrass/BlockDeadBush/BlockMushroom/
//      BlockCrops similarly) inherits its PARENT class's Material;
//   3) Material.java's static initializer, mapping each Material to its
//      MapColor (e.g. wood is built on MapColor.woodColor);
//   4) MapColor.java's 14 MapColor(colorIndex, colorValue) literals.
//
// Ids 73/74 (redstone ore / glowing redstone ore) have no named
// BLOCK_REDSTONE_ORE* enum constant in blocks_data.h (only `blocks[73]`/
// `blocks[74]` array-literal registration in blocks.c) -- used as bare
// literals below with a citing comment.
//
// SPOT-CHECK (>= 15 ids, full chain cited -- CLAUDE.md's decompile-cite
// requirement):
//   id=1  stone:       BlockStone(1,1) -> super(id,tex,Material.rock)
//                       [beta.sh BlockStone:7] -> rock=Material(stoneColor)
//                       [Material.java:114] -> stoneColor=(11,...)
//                       [MapColor.java:16] => colorIndex 11
//   id=2  grass:       BlockGrass(2) -> super(id,Material.grassMaterial)
//                       [beta.sh BlockGrass:7] -> grassMaterial=
//                       Material(grassColor) [Material.java:111] ->
//                       grassColor=(1,...) [MapColor.java:6] => 1
//   id=8/9 water:      Block.java:600-601 explicit `Material.water` ->
//                       MaterialLiquid(waterColor) [Material.java:116] ->
//                       waterColor=(12,...) [MapColor.java:17] => 12
//   id=10/11 lava:     Block.java:602-603 explicit `Material.lava` ->
//                       MaterialLiquid(tntColor) [Material.java:117] ->
//                       tntColor=(4,...) [MapColor.java:9] => 4 (lava
//                       shares tnt's map colour -- see below)
//   id=12 sand:        BlockSand(12,18) -> super(id,tex,Material.sand)
//                       [beta.sh BlockSand:9] -> sand=Material(sandColor)
//                       [Material.java:123] -> sandColor=(2,...)
//                       [MapColor.java:7] => 2
//   id=13 gravel:      BlockGravel(13,19) extends BlockSand, its ctor is a
//                       BARE two-argument super pass-through [beta.sh
//                       BlockGravel:7] -> inherits BlockSand's
//                       Material.sand => colorIndex 2 (NOT dirt -- a
//                       genuine decompile surprise, see below)
//   id=17 log/wood:    BlockLog(17) -> super(id,Material.wood) [beta.sh
//                       BlockLog:7] -> wood=Material(woodColor)
//                       [Material.java:113] -> woodColor=(13,...)
//                       [MapColor.java:18] => 13
//   id=18 leaves:      BlockLeaves(18,52) -> super(id,tex,Material.leaves,
//                       false) [beta.sh BlockLeaves:10] ->
//                       leaves=Material(foliageColor) [Material.java:118]
//                       -> foliageColor=(7,...) [MapColor.java:12] => 7
//   id=20 glass:       Block.java:612 BlockGlass(20,49,Material.glass,
//                       false) -> glass=Material(airColor)
//                       [Material.java:125] -> airColor=(0,...)
//                       [MapColor.java:5] => 0 (transparent on the map,
//                       matching real MC)
//   id=41/42 gold/iron block: BlockOreStorage(41,23)/(42,22) ->
//                       super(id,Material.iron) [beta.sh
//                       BlockOreStorage:5] -> iron=Material(ironColor)
//                       [Material.java:115] -> ironColor=(6,...)
//                       [MapColor.java:11] => 6
//   id=46 tnt:         BlockTNT(46,8) -> super(id,tex,Material.tnt) [beta.sh
//                       BlockTNT:7] -> tnt=Material(tntColor)
//                       [Material.java:126] -> tntColor=(4,...) => 4
//   id=49 obsidian:    BlockObsidian(49,37) extends BlockStone, BARE
//                       two-argument super pass-through [beta.sh
//                       BlockObsidian:7] -> inherits Material.rock =>
//                       colorIndex 11 (plain stone grey, NOT a special
//                       dark colour -- another genuine surprise)
//   id=55 redstoneWire: BlockRedstoneWire(55,164) -> super(id,tex,
//                       Material.circuits) [beta.sh BlockRedstoneWire:13]
//                       -> circuits=MaterialLogic(airColor)
//                       [Material.java:124] => colorIndex 0 (see below)
//   id=78 snow layer:  BlockSnow(78,66) -> super(id,tex,Material.snow)
//                       [beta.sh BlockSnow:7] -> snow=MaterialLogic(
//                       snowColor) [Material.java:129] -> snowColor=
//                       (8,...) [MapColor.java:13] => 8
//   id=80 snow block:  BlockSnowBlock(80,66) -> super(id,tex,
//                       Material.builtSnow) [beta.sh BlockSnowBlock:7] ->
//                       builtSnow=Material(snowColor) [Material.java:130]
//                       => colorIndex 8 (same as the thin layer)
//   id=88 soulSand:    BlockSoulSand(88,104) -> super(id,tex,Material.sand)
//                       [beta.sh BlockSoulSand:5] -> Material.sand =>
//                       colorIndex 2 (sandColor, despite the real block's
//                       brown texture/name -- literal decompile value)
//
// DECOMPILE SURPRISES worth flagging (each verified by the same chain
// above, not assumed):
//   - Lava (10/11) shares TNT's exact map colour (both Material(tntColor)),
//     not its own distinct red -- Beta's `updateMapData` never distinguishes
//     lava from TNT on a rendered map.
//   - Gravel (13) and soul sand (88) both resolve to Material.sand
//     (sandColor), never a "dirt-brown" colour.
//   - Obsidian (49) resolves to plain Material.rock (stoneColor) via its
//     BlockStone parent -- no special obsidian map colour exists in Beta.
//   - Cloth (35/clothColor, index 3) and iron block/iron door (41/71/
//     ironColor, index 6) are DIFFERENT colorIndex values that happen to
//     share the exact same colorValue RGB (10987431) -- see MapColor.java:
//     clothColor=(3,10987431), ironColor=(6,10987431). Visually identical,
//     numerically distinct indices; the table below preserves the real
//     index, not the collapsed RGB.
//   - Cactus (81) and pumpkin/lit-pumpkin (86/91) both resolve to
//     Material(foliageColor) -- they render map-green, like leaves/plants,
//     not their own block colour.
//   - Every id whose Material maps to MapColor.airColor (glass 20, all
//     circuits-material blocks: rail 27/66, detector rail 28, torches
//     50/75/76, fire 51, redstone wire 55, ladder 65, lever 69, button 77,
//     redstone repeater 93/94, portal 90, cake 92) is colorIndex 0 --
//     `updateMapData`'s descent loop treats colorIndex-0 blocks as
//     see-through and keeps scanning DOWN through them for a real surface
//     (see item_map_update_data's descent loop), matching how these are
//     genuinely invisible on a real Beta map.
//
// Ids 0 and 97..255 (unregistered in Beta 1.7.3, plus this port's NET-NEW
// BLOCK_IRON_CHEST=200) default to 0 (air) -- never reached by a real scan
// (the histogram only ever contains ids server_world_get_block can return),
// but a defined, safe default rather than an uninitialized read.
#define MC_AIR 0
#define MC_GRASS 1
#define MC_SAND 2
#define MC_CLOTH 3
#define MC_TNT 4
#define MC_ICE 5
#define MC_IRON 6
#define MC_FOLIAGE 7
#define MC_SNOW 8
#define MC_CLAY 9
#define MC_DIRT 10
#define MC_STONE 11
#define MC_WATER 12
#define MC_WOOD 13

static const uint8_t item_map_block_colors[256] = {
	[BLOCK_STONE] = MC_STONE,
	[BLOCK_GRASS] = MC_GRASS,
	[BLOCK_DIRT] = MC_DIRT,
	[BLOCK_COBBLESTONE] = MC_STONE,		 // Block.java:596 explicit Material.rock
	[BLOCK_PLANKS] = MC_WOOD,				 // Block.java:597 explicit Material.wood
	[BLOCK_SAPLING] = MC_FOLIAGE,			 // BlockSapling extends BlockFlower(plants)
	[BLOCK_BEDROCK] = MC_STONE,			 // Block.java:599 explicit Material.rock
	[BLOCK_WATER_FLOW] = MC_WATER,
	[BLOCK_WATER_STILL] = MC_WATER,
	[BLOCK_LAVA_FLOW] = MC_TNT,			 // MaterialLiquid(tntColor) -- shares TNT's colour
	[BLOCK_LAVA_STILL] = MC_TNT,
	[BLOCK_SAND] = MC_SAND,
	[BLOCK_GRAVEL] = MC_SAND,				 // BlockGravel extends BlockSand (inherits Material.sand)
	[BLOCK_GOLD_ORE] = MC_STONE,			 // BlockOre(*, Material.rock)
	[BLOCK_IRON_ORE] = MC_STONE,
	[BLOCK_COAL_ORE] = MC_STONE,
	[BLOCK_LOG] = MC_WOOD,
	[BLOCK_LEAVES] = MC_FOLIAGE,
	[BLOCK_SPONGE] = MC_CLOTH,				 // BlockSponge(19) -> Material.sponge -> clothColor
	[BLOCK_GLASS] = MC_AIR,				 // Material.glass -> airColor
	[BLOCK_LAPIS_ORE] = MC_STONE,			 // BlockOre(21,...) -> Material.rock
	[BLOCK_LAPIS_CAST] = MC_STONE,			 // Block.java:614 explicit Material.rock
	[BLOCK_DISPENSER] = MC_STONE,			 // BlockDispenser -> Material.rock
	[BLOCK_SANDSTONE] = MC_STONE,			 // BlockSandStone -> Material.rock
	[BLOCK_NOTEBLOCK] = MC_WOOD,			 // BlockNote -> Material.wood
	[BLOCK_BED] = MC_CLOTH,				 // BlockBed -> Material.cloth
	[BLOCK_POWERED_RAIL] = MC_AIR,			 // BlockRail -> Material.circuits
	[BLOCK_DETECTOR_RAIL] = MC_AIR,		 // BlockDetectorRail extends BlockRail
	[BLOCK_PISTON_STICKY] = MC_STONE,		 // BlockPistonBase -> Material.piston -> stoneColor
	[BLOCK_COBWEB] = MC_CLOTH,				 // BlockWeb -> Material.web -> clothColor
	[BLOCK_TALL_GRASS] = MC_FOLIAGE,		 // extends BlockFlower(plants)
	[BLOCK_DEADBUSH] = MC_FOLIAGE,			 // extends BlockFlower(plants)
	[BLOCK_PISTON_BASE] = MC_STONE,
	[BLOCK_PISTON_HEAD] = MC_STONE,		 // BlockPistonExtension -> Material.piston
	[BLOCK_WOOL] = MC_CLOTH,				 // BlockCloth -> Material.cloth
	[BLOCK_PISTON_MOVING] = MC_STONE,		 // BlockPistonMoving -> Material.piston
	[BLOCK_FLOWER] = MC_FOLIAGE,
	[BLOCK_ROSE] = MC_FOLIAGE,
	[BLOCK_BROWM_MUSHROOM] = MC_FOLIAGE,	 // BlockMushroom extends BlockFlower(plants)
	[BLOCK_RED_MUSHROOM] = MC_FOLIAGE,
	[BLOCK_GOLD_CAST] = MC_IRON,			 // BlockOreStorage -> Material.iron
	[BLOCK_IRON_CAST] = MC_IRON,
	[BLOCK_DOUBLE_SLAB] = MC_STONE,		 // BlockStep -> Material.rock
	[BLOCK_SLAB] = MC_STONE,
	[BLOCK_BRICKS] = MC_STONE,				 // Block.java:637 explicit Material.rock
	[BLOCK_TNT] = MC_TNT,
	[BLOCK_BOOKSHELF] = MC_WOOD,			 // BlockBookshelf -> Material.wood
	[BLOCK_MOSSY_COBBLE] = MC_STONE,		 // Block.java:640 explicit Material.rock
	[BLOCK_OBSIDIAN] = MC_STONE,			 // BlockObsidian extends BlockStone (Material.rock)
	[BLOCK_TORCH] = MC_AIR,				 // BlockTorch -> Material.circuits
	[BLOCK_FIRE] = MC_AIR,					 // BlockFire -> Material.fire -> airColor
	[BLOCK_MOB_SPAWNER] = MC_STONE,		 // BlockMobSpawner -> Material.rock
	[BLOCK_WOODEN_STAIRS] = MC_WOOD,		 // BlockStairs(53,planks) -> planks.blockMaterial
	[BLOCK_CHEST] = MC_WOOD,				 // BlockChest -> Material.wood
	[BLOCK_REDSTONE_WIRE] = MC_AIR,		 // Material.circuits
	[BLOCK_DIAMOND_ORE] = MC_STONE,		 // BlockOre -> Material.rock
	[BLOCK_DIAMOND_CAST] = MC_IRON,		 // BlockOreStorage -> Material.iron
	[BLOCK_WORKBENCH] = MC_WOOD,			 // BlockWorkbench -> Material.wood
	[BLOCK_CROPS] = MC_FOLIAGE,			 // BlockCrops extends BlockFlower(plants)
	[BLOCK_FARMLAND] = MC_DIRT,			 // BlockFarmland -> Material.ground
	[BLOCK_FURNACE] = MC_STONE,			 // BlockFurnace -> Material.rock
	[BLOCK_FURNACE_LIT] = MC_STONE,
	[BLOCK_SIGN_POST] = MC_WOOD,			 // BlockSign -> Material.wood
	[BLOCK_DOOR_WOOD] = MC_WOOD,			 // Block.java:656 explicit Material.wood
	[BLOCK_LADDER] = MC_AIR,				 // BlockLadder -> Material.circuits
	[BLOCK_RAIL] = MC_AIR,					 // BlockRail -> Material.circuits
	[BLOCK_STONE_STAIRS] = MC_STONE,		 // BlockStairs(67,cobblestone) -> Material.rock
	[BLOCK_SIGN] = MC_WOOD,				 // BlockSign(68,...,false) -> Material.wood
	[BLOCK_LEVER] = MC_AIR,				 // Material.circuits
	[BLOCK_STONE_PRESSURE_PLATE] = MC_STONE, // Block.java:662 explicit Material.rock
	[BLOCK_DOOR_IRON] = MC_IRON,			 // Block.java:663 explicit Material.iron
	[BLOCK_WOOD_PRESSURE_PLATE] = MC_WOOD,	 // Block.java:664 explicit Material.wood
	// ids 73/74 (redstone ore / glowing redstone ore): no named enum
	// constant in blocks_data.h (blocks.c registers them as bare array
	// literals blocks[73]/blocks[74]). BlockRedstoneOre -> Material.rock.
	[73] = MC_STONE,
	[74] = MC_STONE,
	[BLOCK_REDSTONE_TORCH] = MC_AIR,		 // BlockRedstoneTorch extends BlockTorch (circuits)
	[BLOCK_REDSTONE_TORCH_LIT] = MC_AIR,
	[BLOCK_STONE_BUTTON] = MC_AIR,			 // Material.circuits
	[BLOCK_SNOW] = MC_SNOW,				 // Material.snow -> snowColor
	[BLOCK_ICE] = MC_ICE,					 // BlockIce -> Material.ice
	[BLOCK_SNOW_BLOCK] = MC_SNOW,			 // Material.builtSnow -> snowColor (same as thin layer)
	[BLOCK_CACTUS] = MC_FOLIAGE,			 // Material.cactus -> foliageColor
	[BLOCK_CLAY] = MC_CLAY,				 // BlockClay -> Material.clay
	[BLOCK_REED] = MC_FOLIAGE,				 // BlockReed -> Material.plants
	[BLOCK_JUKEBOX] = MC_WOOD,				 // BlockJukeBox -> Material.wood
	[BLOCK_FENCE] = MC_WOOD,				 // BlockFence -> Material.wood
	[BLOCK_PUMPKIN] = MC_FOLIAGE,			 // Material.pumpkin -> foliageColor
	[BLOCK_NETHERRACK] = MC_STONE,			 // BlockNetherrack -> Material.rock
	[BLOCK_SOULSAND] = MC_SAND,			 // BlockSoulSand -> Material.sand
	[BLOCK_GLOWSTONE] = MC_STONE,			 // Block.java:681 explicit Material.rock
	[BLOCK_PORTAL] = MC_AIR,				 // BlockPortal -> Material.portal -> airColor
	[BLOCK_PUMPKIN_LIT] = MC_FOLIAGE,
	[BLOCK_CAKE] = MC_AIR,					 // Material.cakeMaterial -> airColor
	[BLOCK_REDSTONE_REPEATER] = MC_AIR,	 // Material.circuits
	[BLOCK_REDSTONE_REPEATER_LIT] = MC_AIR,
	[BLOCK_LOCKED_CHEST] = MC_WOOD,		 // BlockLockedChest -> Material.wood
	[BLOCK_TRAP_DOOR] = MC_WOOD,			 // Block.java:688 explicit Material.wood
};

uint8_t item_map_color_index_for_block(uint16_t block_id) {
	if(block_id >= 256)
		return MC_AIR;
	return item_map_block_colors[block_id];
}

// [740c583: MapColor.java] colorValue per colorIndex, decimal RGB packed
// 0xRRGGBB exactly as each `new MapColor(index, colorValue)` literal.
// Encoded here for the 2b render (deferred, see docs/deviations.md); NOT
// consumed by anything in this part-2a slice.
static const uint32_t item_map_color_rgb[14] = {
	[MC_AIR] = 0,		   // airColor     (0, 0)
	[MC_GRASS] = 8368696,  // grassColor   (1, 8368696)
	[MC_SAND] = 16247203,  // sandColor    (2, 16247203)
	[MC_CLOTH] = 10987431, // clothColor   (3, 10987431)
	[MC_TNT] = 16711680,   // tntColor     (4, 16711680)
	[MC_ICE] = 10526975,   // iceColor     (5, 10526975)
	[MC_IRON] = 10987431,  // ironColor    (6, 10987431) -- same RGB as clothColor
	[MC_FOLIAGE] = 31744,  // foliageColor (7, 31744)
	[MC_SNOW] = 16777215,  // snowColor    (8, 16777215)
	[MC_CLAY] = 10791096,  // clayColor    (9, 10791096)
	[MC_DIRT] = 12020271,  // dirtColor    (10, 12020271)
	[MC_STONE] = 7368816,  // stoneColor   (11, 7368816)
	[MC_WATER] = 4210943,  // waterColor   (12, 4210943)
	[MC_WOOD] = 6837042,   // woodColor    (13, 6837042)
};

uint32_t item_map_color_rgb_for_index(uint8_t color_index) {
	if(color_index >= 14)
		return 0;
	return item_map_color_rgb[color_index];
}

// [740c583: MapColor.java] exactly two Materials are liquid in Beta 1.7.3
// (Material.water, Material.lava -- MaterialLiquid instances; every other
// Material's getIsLiquid() returns Material's own default `false`), i.e.
// exactly these four block ids. A direct id-set test stands in for Beta's
// `Block.blocksList[id].blockMaterial.getIsLiquid()` virtual call.
static bool item_map_block_is_liquid(uint16_t id) {
	return id == BLOCK_WATER_FLOW || id == BLOCK_WATER_STILL
		|| id == BLOCK_LAVA_FLOW || id == BLOCK_LAVA_STILL;
}

// World-coordinate block read, treating an unloaded chunk / out-of-range Y
// as air (id 0). [740c583]'s own Chunk.getBlockID has no such fallback --
// Beta always has a synchronously-generatable chunk under `getChunkFrom
// BlockCoords`, an invariant this port's STREAMING chunk loader does not
// share. Falling back to "air" here is the same "keep the pure math safe
// against absent state, prefer a bounded degrade over an assert/crash"
// posture map_data_archive.c's own shape guards already establish -- and
// it self-corrects: an unloaded-at-scan-time column simply reads as
// air/blank this tick and gets re-sampled (possibly correctly, once the
// chunk streams in) whenever the stripe gate revisits that column.
static uint16_t item_map_read_block_id(struct server_world* w, w_coord_t x,
									   w_coord_t y, w_coord_t z) {
	struct block_data blk;
	if(!server_world_get_block(w, x, y, z, &blk))
		return BLOCK_AIR;
	return blk.type;
}

// Defensive iteration cap on the per-sub-block surface descent below.
// Beta's own `while(true)` loop (ItemMap.updateMapData) has no explicit
// bound either -- it relies on a REAL Beta world's bedrock floor always
// producing a non-zero, non-air-colored block id before the loop could run
// away. This port's synthetic/void-column test fixtures are not guaranteed
// to carry that invariant, so a generous bound (comfortably above the
// world's 0..127 vertical span) turns a hypothetical hang into a bounded,
// harmless "found nothing, id stays 0" outcome. Never expected to trigger
// against any real generated chunk (this is an engineering safety net, not
// a faithfulness deviation -- Beta's own termination depends on the exact
// same real-world assumption).
#define ITEM_MAP_DESCENT_GUARD 256

static bool item_map_scan(struct server_world* w, struct map_data* md,
	double entity_x, double entity_z, enum world_dim world_dim) {
	assert(w && md);

	// [740c583: ItemMap.updateMapData] `if (worldProvider.worldType ==
	// mapData.dimension) { ... }`
	if(!map_data_scan_should_update(world_dim, md->dimension))
		return false;

	int32_t blocks_per_pixel = map_data_scan_blocks_per_pixel(md->scale);
	// [740c583] `worldProvider.hasNoSky` -- true only in the Nether, the
	// only non-overworld dimension this port has.
	bool has_no_sky = (world_dim == WORLD_DIM_NETHER);
	int32_t px_center
		= map_data_scan_pixel_center(entity_x, md->x_center, blocks_per_pixel);
	int32_t pz_center
		= map_data_scan_pixel_center(entity_z, md->z_center, blocks_per_pixel);
	int32_t radius = map_data_scan_radius(blocks_per_pixel, has_no_sky);

	// [740c583] `++var3.step;` -- once per call, matching map_data.h's
	// documented "caller owns the increment" contract.
	md->step++;

	bool any_changed = false;

	for(int32_t px = px_center - radius + 1; px < px_center + radius; px++) {
		if(!map_data_scan_stripe_selected(px, md->step))
			continue;

		// Beta's var13/var14 (dirty_min/dirty_max) feed ONLY setColumnDirty's
		// per-column min/max tracking, which this port does not implement
		// (see the doc comment at the bottom of this outer loop) -- so
		// there is nothing to track here beyond `any_changed` itself.
		double prev_avg_height = 0.0; // var15

		for(int32_t pz = pz_center - radius - 1; pz < pz_center + radius;
			pz++) {
			if(!(px >= 0 && pz >= -1 && px < MAP_DATA_DIM
				 && pz < MAP_DATA_DIM))
				continue;

			int32_t dx = px - px_center; // Beta dx
			int32_t dz = pz - pz_center; // Beta dz
			// edge flag: true = in the fading/dither edge band (outside the
			// (radius-2) inner circle).
			bool edge_band = dx * dx + dz * dz > (radius - 2) * (radius - 2);
			// [740c583] `(var7/var6 + var12 - var4/2) * var6` -- Java
			// int/int truncating division on the map CENTER (not an
			// entity position, so map_data_scan_pixel_center's
			// java_floor_double does not apply here); C's `/` on int32_t
			// truncates toward zero identically to Java's `/`.
			int32_t world_x = (md->x_center / blocks_per_pixel + px
							   - MAP_DATA_DIM / 2)
				* blocks_per_pixel;
			int32_t world_z = (md->z_center / blocks_per_pixel + pz
							   - MAP_DATA_DIM / 2)
				* blocks_per_pixel;

			uint16_t histogram[256];
			memset(histogram, 0, sizeof(histogram));
			int32_t water_depth = 0; // var30
			double avg_height = 0.0; // var31

			if(has_no_sky) {
				// [740c583: hasNoSky branch] Nether column pick -- part-1
				// helper; weight 10, flat avg_height 100.0 (no sub-block
				// loop at all in this branch).
				bool is_stone
					= map_data_scan_nether_column_is_stone(world_x, world_z);
				uint16_t picked = is_stone ? BLOCK_STONE : BLOCK_DIRT;
				histogram[picked] += 10;
				avg_height = 100.0;
			} else {
				for(int32_t sx = 0; sx < blocks_per_pixel; sx++) {
					for(int32_t sz = 0; sz < blocks_per_pixel; sz++) {
						uint8_t ground_height = 0;
						// Unloaded column: treat as height 0 (see
						// item_map_read_block_id's identical fallback
						// posture -- self-corrects on a later revisit).
						server_world_get_height(w, world_x + sx,
												world_z + sz, &ground_height);
						// [740c583] `var35 = getHeightValue(...) + 1;` --
						// server_world_get_height ALREADY equals Beta's
						// getHeightValue (top opaque block + 1, per its own
						// doc comment), so this adds the SAME second +1
						// the decompile does, not a duplicate of the one
						// already baked into getHeightValue.
						int32_t y = (int32_t)ground_height + 1;
						uint16_t block_id = 0; // var36

						if(y > 1) {
							bool descent_done = false;
							for(int guard = 0;
								guard < ITEM_MAP_DESCENT_GUARD && !descent_done;
								guard++) {
								bool found = true; // var37
								block_id = item_map_read_block_id(
									w, world_x + sx, y - 1,
									world_z + sz);
								if(block_id == BLOCK_AIR) {
									found = false;
								} else if(y > 0
										  && item_map_color_index_for_block(
												 block_id)
											 == MC_AIR) {
									found = false;
								}
								if(!found) {
									y--;
									block_id = item_map_read_block_id(
										w, world_x + sx, y - 1,
										world_z + sz);
								}
								if(found) {
									if(block_id == BLOCK_AIR
									   || !item_map_block_is_liquid(
											  block_id)) {
										descent_done = true;
									} else {
										// [740c583] water-depth walk:
										// `break label164` exits BOTH loops
										// at once.
										int32_t wy = y - 1; // var38
										for(int wguard = 0;
											wguard < ITEM_MAP_DESCENT_GUARD;
											wguard++) {
											uint16_t under
												= item_map_read_block_id(
													w, world_x + sx,
													wy, world_z + sz);
											wy--;
											water_depth++;
											if(wy <= 0
											   || under == BLOCK_AIR
											   || !item_map_block_is_liquid(
													  under)) {
												break;
											}
										}
										descent_done = true;
									}
								}
							}
						}

						avg_height += (double)y
							/ (double)(blocks_per_pixel * blocks_per_pixel);
						histogram[block_id]++;
					}
				}
			}

			water_depth /= blocks_per_pixel * blocks_per_pixel; // var30

			// [740c583] Beta's own `int var23=0; int var24=0; int var25=0;`
			// (declared, never reassigned in the loop body) plus the three
			// `var10000 = varNN / (var6*var6)` lines that follow are
			// provably-inert decompiler dead code (each divides a value
			// that is always 0, stores it into a local immediately
			// overwritten by the next line, and the final value is never
			// read again) -- omitted here with zero observable effect.

			int32_t best_count = 0;   // var33
			uint16_t best_id = 0;	   // var34
			for(int32_t id = 0; id < 256; id++) {
				if(histogram[id] > best_count) {
					best_id = (uint16_t)id;
					best_count = histogram[id];
				}
			}

			// [740c583] brightness banding.
			double shade = (avg_height - prev_avg_height) * 4.0
					/ (double)(blocks_per_pixel + 4)
				+ ((double)((px + pz) & 1) - 0.5) * 0.4; // var41
			uint8_t shade_band = 1;						 // var42
			if(shade > 0.6)
				shade_band = 2;
			if(shade < -0.6)
				shade_band = 0;

			uint8_t color_index = 0; // var38
			if(best_id > 0) {
				uint8_t mc = item_map_color_index_for_block(best_id); // var44
				if(mc == MC_WATER) {
					// [740c583] water-specific shading override.
					shade = (double)water_depth * 0.1
						+ (double)((px + pz) & 1) * 0.2;
					shade_band = 1;
					if(shade < 0.5)
						shade_band = 2;
					if(shade > 0.9)
						shade_band = 0;
				}
				color_index = mc;
			}

			prev_avg_height = avg_height; // var15 = var31 (BEFORE the gate,
										  // matching the decompile's line
										  // order exactly)

			if(pz >= 0 && dx * dx + dz * dz < radius * radius
			   && (!edge_band || ((px + pz) & 1) != 0)) {
				uint8_t* slot = &md->colors[px + pz * MAP_DATA_DIM];
				uint8_t new_color = (uint8_t)(color_index * 4 + shade_band);
				if(*slot != new_color) {
					*slot = new_color;
					// [740c583] when the column min <= max,
					// the map data marks that column dirty -- Beta's
					// min and max track this COLUMN's min/max changed row
					// purely to feed setColumnDirty's per-column
					// dirty-rectangle bookkeeping (multiplayer incremental
					// sync only, see item_map.h's doc comment). The single
					// persisted effect setColumnDirty has for this SP-only
					// port is super.markDirty() -- "at least one pixel
					// changed this call" -- which `any_changed` already
					// captures with no need for the per-column range.
					any_changed = true;
				}
			}
		}
	}

	return any_changed;
}

/* ItemMap.func_28011_a samples the same World whose provider gates the scan. */
bool item_map_update_data_w(struct server_world* w, struct map_data* md,
	double entity_x, double entity_z) {
	assert(w && md);
	return item_map_scan(w, md, entity_x, entity_z, w->dimension);
}

bool item_map_update_data(struct server_local* s, struct map_data* md,
	double entity_x, double entity_z, enum world_dim world_dim) {
	assert(s && md);
	return item_map_scan(&s->worlds[0], md, entity_x, entity_z, world_dim);
}

// ============================================================================
// Resident map-data cache
// ============================================================================

#ifdef CAVEWII_TESTING
static unsigned item_map_victim_save_attempts;
unsigned item_map_test_victim_save_attempts(void) {
	return item_map_victim_save_attempts;
}
#endif

static int item_map_cache_find(struct server_local* s, int32_t id) {
	for(int i = 0; i < MAX_MAP_CACHE; i++) {
		if(s->map_cache_id[i] == id)
			return i;
	}
	return -1;
}

static int item_map_cache_reserve_slot(struct server_local* s) {
	// Prefer a genuinely free slot.
	for(int i = 0; i < MAX_MAP_CACHE; i++) {
		if(s->map_cache_id[i] == INT32_MIN)
			return i;
	}
	// Otherwise evict the least-recently-touched resident entry, flushing
	// it first if dirty -- never silently drop unsaved colour data.
	int victim = 0;
	for(int i = 1; i < MAX_MAP_CACHE; i++) {
		if(s->map_cache_touch[i] < s->map_cache_touch[victim])
			victim = i;
	}
	if(s->map_cache_dirty[victim]) {
#ifdef CAVEWII_TESTING
		item_map_victim_save_attempts++;
#endif
		if(!map_data_archive_save(s->level_name, s->map_cache_id[victim],
								   &s->map_cache_data[victim]))
			return -1;
		s->map_cache_dirty[victim] = false;
	}
	return victim;
}

struct map_data* item_map_cache_resident(struct server_local* s, int32_t id) {
	assert(s && id >= INT16_MIN && id <= INT16_MAX);
	int slot = item_map_cache_find(s, id);
	if(slot < 0)
		return NULL;
	s->map_cache_touch[slot] = ++s->map_cache_touch_counter;
	return &s->map_cache_data[slot];
}

bool item_map_cache_install(struct server_local* s, int32_t id,
							const struct map_data* md, bool dirty) {
	assert(s && md && id >= INT16_MIN && id <= INT16_MAX);
	int slot = item_map_cache_find(s, id);
	if(slot < 0)
		slot = item_map_cache_reserve_slot(s);
	if(slot < 0)
		return false;
	s->map_cache_holder_count[slot] = 0;
	s->map_cache_data[slot] = *md;
	s->map_cache_id[slot] = id;
	s->map_cache_dirty[slot] = dirty;
	s->map_cache_touch[slot] = ++s->map_cache_touch_counter;
	return true;
}

void item_map_cache_mark_dirty(struct server_local* s, int32_t id) {
	int slot = item_map_cache_find(s, id);
	assert(slot >= 0); // caller must have just resident/install'd this id
	if(slot >= 0)
		s->map_cache_dirty[slot] = true;
}

void item_map_cache_flush_all(struct server_local* s) {
	assert(s);
	for(int i = 0; i < MAX_MAP_CACHE; i++) {
		if(s->map_cache_id[i] != INT32_MIN && s->map_cache_dirty[i]) {
			if(map_data_archive_save(s->level_name, s->map_cache_id[i],
									 &s->map_cache_data[i]))
				s->map_cache_dirty[i] = false;
			// A failed save leaves the entry dirty -- retried at the next
			// flush boundary, same "stays modified, retries later" posture
			// server_local.c's own autosave already documents for
			// level.dat.
		}
	}
}

// ============================================================================
// ItemMap onCreated / onUpdate dispatch
// ============================================================================

#define ITEM_MAP_RESERVE_RETRY_TICKS 100 /* 5 s at 20 server ticks/s */
#define ITEM_MAP_RETRY_SLOTS 128
struct item_map_retry {
	struct server_local* server;
	int32_t id;
	uint64_t failed_at;
};
static struct item_map_retry item_map_retries[ITEM_MAP_RETRY_SLOTS];
static uint64_t item_map_overflow_failed_at;
static bool item_map_overflow_active;

static struct item_map_retry* item_map_retry_find(struct server_local* s,
											 int32_t id) {
	for(size_t i = 0; i < ITEM_MAP_RETRY_SLOTS; i++) {
		struct item_map_retry* retry = &item_map_retries[i];
		if(retry->server == s && retry->id == id) {
			/* The same server object can be reused for a newly loaded world. */
			if(s->world_time < retry->failed_at) {
				retry->server = NULL;
				return NULL;
			}
			return retry;
		}
	}
	return NULL;
}

static bool item_map_retry_waiting(struct server_local* s, int32_t id) {
	struct item_map_retry* retry = item_map_retry_find(s, id);
	if(retry)
		return s->world_time - retry->failed_at < ITEM_MAP_RESERVE_RETRY_TICKS;
	for(size_t i = 0; i < ITEM_MAP_RETRY_SLOTS; i++) {
		retry = &item_map_retries[i];
		if(!retry->server || s->world_time < retry->failed_at
		   || s->world_time - retry->failed_at >= ITEM_MAP_RESERVE_RETRY_TICKS)
			return false;
	}
	return item_map_overflow_active
		&& s->world_time >= item_map_overflow_failed_at
		&& s->world_time - item_map_overflow_failed_at < ITEM_MAP_RESERVE_RETRY_TICKS;
}

static void item_map_retry_failed(struct server_local* s, int32_t id) {
	struct item_map_retry* retry = item_map_retry_find(s, id);
	if(retry) {
		retry->failed_at = s->world_time;
		return;
	}
	for(size_t i = 0; i < ITEM_MAP_RETRY_SLOTS; i++) {
		retry = &item_map_retries[i];
		if(!retry->server || s->world_time < retry->failed_at
			   || s->world_time - retry->failed_at >= ITEM_MAP_RESERVE_RETRY_TICKS) {
			*retry = (struct item_map_retry){s, id, s->world_time};
			fprintf(stderr, "[map] cache reserve failed for map %d; retrying after %d ticks\n",
					(int)id, ITEM_MAP_RESERVE_RETRY_TICKS);
			return;
		}
	}
	/* All 128 records are cooling down. Keep a global bound on excess maps. */
	if(!item_map_overflow_active || s->world_time < item_map_overflow_failed_at
	   || s->world_time - item_map_overflow_failed_at >= ITEM_MAP_RESERVE_RETRY_TICKS) {
		item_map_overflow_failed_at = s->world_time;
		item_map_overflow_active = true;
		fprintf(stderr, "[map] cache reserve failed; excess maps cooling down\n");
	}
}

// [740c583: ItemMap.onCreated] the stack damage takes the world's next unique
// "map" data id; the new map data is centred on floor(player.posX) and
// floor(player.posZ), at scale 3, in the player's dimension,
// then marked dirty.
//
// FIX (id-reuse-on-persistence-failure): map_idcounts_get_unique_id can now
// fail (map_idcounts.h's durability contract -- see that header) when
// idcounts.dat cannot be durably written. Returning false here tells the
// caller (inventory_crafting.c/inventory_player.c's CRAFTING_SLOT_OUTPUT
// branch, or server_local.c's map_demo debug rig) to fail the craft/give
// outright instead of handing out an ITEM_MAP stack with no durably-
// reserved id -- exactly the "no map assigned" behavior CLAUDE.md's
// no-silent-shortcuts rule calls for, rather than silently continuing with
// an id the on-disk counter never actually reserved.
static bool item_map_on_created(struct server_local* s,
								struct server_player* actor,
								struct item_data* stack) {
	assert(s && stack);
	/* S1 (R-S1a1b F3): Beta's `EntityPlayer var3` is the CRAFTER, and
	 * `var5.xCenter = floor(player.posX)` centres the fresh map on that
	 * player. S1-a1b admitted the guest's workbench window, which made the
	 * 3x3 map recipe reachable for the split guest -- and this stamped
	 * s->player regardless, so a guest-crafted map came out centred on the
	 * HOST. The craft dispatch (inventory_click.c) passes the inventory's
	 * owning body. A NULL actor means an inventory with no owner (client
	 * prediction mirrors never reach this server-only leg; harness
	 * inventories can), where the host is the only body that provably
	 * exists -- the pre-S1 behaviour, unchanged. */
	const struct server_player* crafter = actor ? actor : &s->player;

	int32_t id;
	if(!map_idcounts_get_unique_id(s->level_name, "map", &id))
		return false;
	// Java short->int widening on getUniqueDataId's return, then Beta's
	// setItemDamage(int) narrows into the ItemStack's damage field; this
	// port's item_data.durability is uint16_t (items.h), so the low 16
	// bits are preserved identically for the entire realistic id range.

	// Wii server-thread stack budget (source/main.c:925 precedent: the
	// `static struct server_local server` there, for the same reason).
	// struct map_data carries the 128x128 colour buffer (map_data.h,
	// ~16.4KB) -- as a stack local this one field alone pushed this
	// function's measured frame to 16,464B against the server thread's
	// 64KB THREAD_STACK_SIZE (platform/thread.c:220), ~26% of the whole
	// budget for one call frame. AUDITED single-thread contract: this
	// function runs ONLY on the server thread -- every call site
	// (server_local.c:15781's map_demo_defer path, and the crafting-output
	// dispatch in inventory_crafting.c/inventory_player.c's
	// `out_def->onCreated(...)`, itself only reachable through a
	// server-owned inventory's `inv->logic->pre_action`, inventory.c:350 --
	// client-side prediction windows in window_container.c create their
	// inventories with `logic == NULL` and never fire pre_action) resolves
	// to server_local_update, which server_local.c:90's own comment
	// documents as running "only ever ... from server_local_thread outside
	// tests". No recursion/re-entrancy: server_local_update ticks
	// sequentially and this function never calls itself or is called
	// re-entrantly. static, not a stack local, keeps the buffer off this
	// frame entirely.
	static struct map_data md;
	map_data_set_defaults(&md, java_floor_double(crafter->x),
						  java_floor_double(crafter->z),
						  (int8_t)crafter->dimension);
	// map_data_set_defaults already sets scale=3 [740c583, cited in
	// map_data.c] -- matches ItemMap.onCreated's own `var5.scale = 3;`.
	if(!item_map_cache_install(s, id, &md, true)) // markDirty()
		return false;
	stack->durability = (uint16_t)id;
	return true;
}

// [740c583: ItemMap.onUpdate] it fetches the stack's map data; for a player
// holder it updates the visible players; and for the held stack
// it runs updateMapData -- gated on
// a non-multiplayer world; this engine's server-local player IS the
// SP-only world the guard selects for, so no separate runtime check is
// needed here (there is no multiplayer world for this dispatch to run
// against).
static void item_map_on_update_in(struct server_local* s,
	struct server_world* w, struct server_player* actor,
							   struct item_data* stack, size_t slot,
							   bool is_held) {
	assert(s && actor && stack);
	assert(w);
	(void)slot;

	// Wii server-thread stack budget (source/main.c:925 precedent, and see
	// item_map_on_created's identical-shape comment above for the full
	// single-thread-contract citation -- this function's own callers are
	// server_local_tick_item_updates, called at server_local.c:15155,
	// itself inside server_local_update). Beta's `loaded`/`fresh` locals
	// below were EACH a full struct map_data (~16.4KB, the 128x128 colour
	// buffer) as stack locals; together they measured a 32,856B frame, ~51%
	// of the server thread's 64KB THREAD_STACK_SIZE (platform/thread.c:220)
	// for one call. Live-range audit: the `if`/`else` below is the ONLY use
	// of either -- `loaded` is read (map_data_archive_load) then handed
	// straight to item_map_cache_install (which copies it into the cache
	// slot) and never touched again; `fresh` is populated
	// (map_data_set_defaults) and likewise consumed by its own
	// item_map_cache_install call. The two branches are mutually exclusive
	// (never both taken in one call), so their live ranges never overlap --
	// ONE function-scope static scratch record, reused for both, is enough.
	// static, not a stack local, keeps this off the frame entirely.
	static struct map_data scratch;

	// [740c583: ItemMap.getMapData] the world's item data is loaded under
	// the name "map_" plus the stack damage; when that is missing,
	// the stack damage takes a fresh unique "map" id, and a
	// spawn-centered default is created and marked dirty -- the resident-cache hit
	// check (item_map_cache_resident) mirrors MapStorage.loadedDataMap's
	// own cache-hit short-circuit staying separate from this fallback.
	/* [740c583 ItemStack:83-85, ItemMap:24-27]: Damage is signed TAG_SHORT. */
	int32_t id = (int16_t)stack->durability;
	int32_t original_id = id;
	struct map_data* md = item_map_cache_resident(s, id);
	if(!md) {
		if(item_map_retry_waiting(s, id))
			return;
		if(map_data_archive_load(s->level_name, id, &scratch)) {
			// File exists on disk under this exact id -- populate the
			// cache from it (clean: matches disk, nothing to flush).
			if(!item_map_cache_install(s, id, &scratch, false)) {
				item_map_retry_failed(s, id);
				return;
			}
		} else {
			// [740c583: getMapData's fallback] no file for THIS id --
			// allocate a fresh one and re-stamp the stack's own damage,
			// centered on WORLD SPAWN (not player position -- the
			// deliberate difference from onCreated's default; see
			// map_data.h's doc comment on the two call sites).
			//
			// FIX (id-reuse-on-persistence-failure): if the durable
			// allocation itself fails (map_idcounts.h's contract), do NOT
			// stamp an unreserved id onto `stack` -- that is exactly the bug
			// this fix closes. There is no "fail the craft" hook available
			// here (unlike onCreated, this runs from the per-tick item-update
			// dispatch, not a one-shot crafting pickup), so simply leave
			// `stack`/`id` unchanged and skip the rest of this call: the
			// backing file is still missing next tick, so this exact fallback
			// branch runs again and retries the allocation -- the same
			// self-healing retry posture map_idcounts.c's own header comment
			// documents for a lost allocation, just deferred a tick instead of
			// ever being silently reused.
			int32_t new_id;
			if(!map_idcounts_get_unique_id(s->level_name, "map", &new_id))
				return;
			map_data_set_defaults(&scratch, actor->spawn_x,
								  actor->spawn_z,
								  (int8_t)w->dimension);
			if(!item_map_cache_install(s, new_id, &scratch, true)) { // markDirty()
				item_map_retry_failed(s, id);
				return;
			}
			stack->durability = (uint16_t)new_id;
			id = new_id;
		}
		struct item_map_retry* retry = item_map_retry_find(s, original_id);
		if(retry)
			retry->server = NULL;
		md = item_map_cache_resident(s, id);
		assert(md);
	}

	/* Beta src_original MapData.java:71-104: append the updater first,
	 * then remove dead/non-carrying players. Dimension/range only hide an
	 * icon; they do NOT remove its updater entry. Runs for non-held maps too. */
	int cache_slot = item_map_cache_find(s, id);
	uint8_t* order = s->map_cache_holders[cache_slot];
	uint64_t* generations = s->map_cache_holder_generation[cache_slot];
	uint8_t* count = &s->map_cache_holder_count[cache_slot];
	unsigned updater = actor == &s->player ? 0 : 1;
	uint64_t generation = updater ? s->guest_generation : 0;
	struct server_player* present[SERVER_LOCAL_MAX_PRESENT_ACTORS];
	size_t present_count = server_local_present_actors(s, present,
		SERVER_LOCAL_MAX_PRESENT_ACTORS);
	/* Retire a departed admission before adding its replacement to the tail. */
	for(unsigned h = 0; h < *count;) {
		if(order[h] == 1 && generations[h] != s->guest_generation) {
			memmove(order + h, order + h + 1, *count - h - 1);
			memmove(generations + h, generations + h + 1,
				(*count - h - 1) * sizeof(*generations));
			--*count;
		} else {
			h++;
		}
	}
	bool registered = false;
	for(unsigned h = 0; h < *count; h++)
		registered |= order[h] == updater;
	if(!registered) {
		assert(*count < SERVER_LOCAL_MAX_PRESENT_ACTORS);
		order[*count] = updater;
		generations[*count] = generation;
		++*count;
	}
	struct client_rpc call = {.type = CRPC_MAP_MARKERS};
	call.payload.map_markers.map_id = id;
	call.payload.map_markers.length = 1;
	call.payload.map_markers.bytes[0] = 1;
	for(unsigned h = 0; h < *count;) {
		struct server_player* holder = order[h] ? s->guest_actor : &s->player;
		/* getMapData may have changed this dispatch copy's id before the
		 * caller writes it back; Java mutates the inventory stack in place. */
		bool carries = holder == actor;
		for(size_t n = 0; n < present_count; n++) {
			if(present[n] != holder || (order[h] && !s->guest_gameplay))
				continue;
			for(size_t k = INVENTORY_SLOT_MAIN; k < holder->inventory.capacity; k++) {
				struct item_data held;
				if(inventory_get_slot(&holder->inventory, k, &held)
				   && held.id == ITEM_MAP && held.count == stack->count
				   && (int16_t)held.durability == id) {
					carries = true;
					break;
				}
			}
		}
		if(!carries || holder->health <= 0) {
			memmove(order + h, order + h + 1, *count - h - 1);
			memmove(generations + h, generations + h + 1,
				(*count - h - 1) * sizeof(*generations));
			--*count;
			/* MapData.java:81-104 increments its list index after removal,
			 * skipping the shifted holder's icon for this one update. */
			h++;
			continue;
		}
		struct item_map_marker marker;
		/* Beta MapData.java:91: the UPDATING player's yaw for every icon. */
		if(item_map_player_marker(md, holder->x, holder->z, actor->rx,
			(int8_t)holder->dimension, true, &marker)) {
			unsigned off = call.payload.map_markers.length;
			call.payload.map_markers.bytes[off] = (uint8_t)((marker.rotation & 15) * 16);
			call.payload.map_markers.bytes[off + 1] = (uint8_t)marker.x;
			call.payload.map_markers.bytes[off + 2] = (uint8_t)marker.y;
			call.payload.map_markers.length += 3;
		}
		h++;
	}
	if(is_held)
		(void)server_local_try_send_w(w, call);

	// [740c583] `if (var5) { this.updateMapData(...); }` -- var5 is Beta's
	// isCurrentItem/is_held.
	if(is_held) {
		if(item_map_update_data_w(w, md, actor->x, actor->z))
			item_map_cache_mark_dirty(s, id);
	}
}

static void item_map_on_update_for(struct server_local* s,
	struct server_player* actor, struct item_data* stack, size_t slot,
	bool is_held) {
	struct server_world* w = server_local_world_of(s, actor);
	if(w)
		item_map_on_update_in(s, w, actor, stack, slot, is_held);
}

/* Legacy single-player entry; inventory dispatch supplies its actual owner. */
static void item_map_on_update(struct server_local* s, struct item_data* stack,
	size_t slot, bool is_held) {
	item_map_on_update_in(s, &s->worlds[0], &s->player, stack, slot, is_held);
}

struct item item_map = {
	.name = "Map",
	// The damage field carries an OPAQUE map id, never tool/weapon wear --
	// has_damage=false is this engine's established convention for that
	// shape (item_dye.c's damage-as-colour precedent: gui_util.c's
	// durability-bar overlay is the ONLY consumer of has_damage, and it
	// would otherwise divide by max_damage==0 -- a real bug, not merely a
	// cosmetic mismatch -- since a map item never sets max_damage).
	.has_damage = false,
	.max_stack = 1,		// [740c583: ItemMap ctor] setMaxStackSize(1)
	.fuel = 0,
	.renderItem = render_item_map, // 2b in-hand render LANDED 2026-08-26:
								   // live 128x128 first-person draw
								   // (render_item.c); every other env keeps
								   // the static sprite, exactly as Beta
								   // [740c583 ItemRenderer]
	.onCreated = item_map_on_created,
	.onUpdate = item_map_on_update,
	.onUpdateFor = item_map_on_update_for,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 12,
			.texture_y = 3,
		},
	},
};
