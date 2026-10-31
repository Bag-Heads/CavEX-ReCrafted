/*
	DECORATION PLACEMENT SWEEP (card DECOR/1, intent docs/intent/decoration-oracle.md)

	test_oracle_worldgen proves TERRAIN byte-exact against mc173, but nothing
	checked the populate pass (lakes, dungeons, clay, ores, trees, flowers,
	mushrooms, tall grass, dead bushes, sugar cane, pumpkins, cacti, springs,
	snow). Natural sugar cane on sand lived since the first port because of
	that gap (tmp/jobs/split-dimensions-plan-0924/out/REED-SAND-DIAG-0926.md).

	What this does: for several seeds it generates REAL terrain (worldgen_
	generate_chunk) for 8x8-chunk regions (a fixed site plus one region per
	biome group found by a deterministic spiral search), runs worldgen_populate
	+ the snow pass over each 7x7 interior in raster order (the offline shape
	the reed diagnosis used), and checks EVERY block write the populate pass
	makes against the Beta 1.7.3 rule that gates that write -- evaluated on the
	world state at the moment of the write, which is exactly the state Beta's
	own check sees (populate writes use setBlock without neighbour
	notification, so a later write never removes an earlier plant in Beta
	either; checking the final world would report false positives).

	The rules below are an INDEPENDENT port from the decompile (never from
	decorate.c/tree.c), each cited to ../mcb173-retromcp/minecraft/src_original/
	net/minecraft/src/<File>.java:<line>. Coverage table + unchecked clauses:
	tmp/jobs/split-dimensions-plan-0924/out/DECOR-COVERAGE-0926.md.

	REED_ON_SAND: before codex/reed-0926 (a3c02638) CaveWii's
	dec_can_stay(DEC_SUGARCANE) accepted SAND below a reed; Beta's
	BlockReed.canPlaceBlockAt does not. That clause is still reported
	separately (so a regression names itself), but since G4 integrated the reed
	fix it is STRICT like every other clause: any hit is a hard FAIL. The old
	EXPECTED-FAIL carve-out was removed at G4 assembly (2026-09-26).

	Not checked (documented in the coverage doc): the LIGHT terms of
	BlockFlower.canBlockStay (light>=8 || sees sky) and BlockMushroom.
	canBlockStay (light<13) -- both read World.getFullBlockLightValue, which in
	Beta depends on the time of day the chunk populated (skylightSubtracted)
	and on populate-time lighting CaveWii does not model, so there is no
	deterministic Beta answer to assert. Informational counts are printed.
	Under-placement (CaveWii refusing a spot Beta accepts) is invisible to a
	write-time check; those divergences come from static review (coverage doc).
*/

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "network/server_world.h"
#include "worldgen/biome.h"
#include "worldgen/decorate.h"
#include "worldgen/populate.h"
#include "worldgen/worldgen.h"

// --- stubbed external dependencies (same as test_worldgen) ----------------
struct block;
struct block* blocks[256] = {0};

uint8_t nibble_read(uint8_t* base, size_t idx) {
	return (idx & 1) ? (uint8_t)(base[idx >> 1] >> 4)
					 : (uint8_t)(base[idx >> 1] & 0x0F);
}
void nibble_write(uint8_t* base, size_t idx, uint8_t data) {
	size_t i = idx >> 1;
	if(idx & 1)
		base[i] = (uint8_t)((base[i] & 0x0F) | ((data & 0x0F) << 4));
	else
		base[i] = (uint8_t)((base[i] & 0xF0) | (data & 0x0F));
}
// -------------------------------------------------------------------------

// Beta block ids (raw, so the rules below read against the decompile).
enum {
	B_AIR = 0,
	B_STONE = 1,
	B_GRASS = 2,
	B_DIRT = 3,
	B_COBBLE = 4,
	B_SAPLING = 6,
	B_WATER_FLOW = 8,
	B_WATER = 9,
	B_LAVA_FLOW = 10,
	B_LAVA = 11,
	B_SAND = 12,
	B_GRAVEL = 13,
	B_GOLD_ORE = 14,
	B_IRON_ORE = 15,
	B_COAL_ORE = 16,
	B_LOG = 17,
	B_LEAVES = 18,
	B_GLASS = 20,
	B_LAPIS_ORE = 21,
	B_TALLGRASS = 31,
	B_DEADBUSH = 32,
	B_DANDELION = 37,
	B_ROSE = 38,
	B_MUSH_BROWN = 39,
	B_MUSH_RED = 40,
	B_MOSSY = 48,
	B_TORCH = 50,
	B_FIRE = 51,
	B_SPAWNER = 52,
	B_CHEST = 54,
	B_REDSTONE_WIRE = 55,
	B_DIAMOND_ORE = 56,
	B_CROPS = 59,
	B_FARMLAND = 60,
	B_REDSTONE_ORE = 73,
	B_SNOW_LAYER = 78,
	B_ICE = 79,
	B_CACTUS = 81,
	B_CLAY = 82,
	B_REED = 83,
	B_PUMPKIN = 86,
	B_PORTAL = 90,
};

// Beta Material.isSolid() [Material.java]: false only for MaterialTransparent
// (air, fire), MaterialLiquid (water, lava), MaterialLogic (plants, circuits,
// snow layer) and MaterialPortal; true for everything else (leaves, ice, glass,
// cactus, pumpkin, ...). Restricted to ids that exist in a populated world.
static bool mat_solid(uint8_t id) {
	switch(id) {
		case B_AIR:
		case B_FIRE:
		case B_WATER_FLOW:
		case B_WATER:
		case B_LAVA_FLOW:
		case B_LAVA:
		case B_SAPLING:
		case B_TALLGRASS:
		case B_DEADBUSH:
		case B_DANDELION:
		case B_ROSE:
		case B_MUSH_BROWN:
		case B_MUSH_RED:
		case B_TORCH:
		case B_REDSTONE_WIRE:
		case B_CROPS:
		case B_SNOW_LAYER:
		case B_REED:
		case B_PORTAL: return false;
		default: return true;
	}
}
static bool mat_liquid(uint8_t id) {
	return id == B_WATER_FLOW || id == B_WATER || id == B_LAVA_FLOW
		|| id == B_LAVA;
}
static bool mat_water(uint8_t id) {
	return id == B_WATER_FLOW || id == B_WATER;
}

// Beta Block.opaqueCubeLookup[] as the static table stands during worldgen:
// Block.isOpaqueCube() defaults true [Block.java:271]; overridden false by
// BlockBreakable:11 (glass, ice), BlockCactus:48, BlockFlower:47 (all plants
// incl. mushrooms / tall grass / dead bush / sapling), BlockFluid:52,
// BlockMobSpawner:22, BlockReed:62, BlockSnow:17, BlockTorch:15,
// BlockRedstoneWire:25, BlockFarmland:18, BlockFire:35, BlockPortal:29. LEAVES
// read TRUE in the table (Block.java:151 samples BlockLeaves.isOpaqueCube =
// !graphicsLevel before the graphics level is set -- see populate.h).
static bool beta_opaque_cube(uint8_t id) {
	switch(id) {
		case B_AIR:
		case B_SAPLING:
		case B_WATER_FLOW:
		case B_WATER:
		case B_LAVA_FLOW:
		case B_LAVA:
		case B_GLASS:
		case B_TALLGRASS:
		case B_DEADBUSH:
		case B_DANDELION:
		case B_ROSE:
		case B_MUSH_BROWN:
		case B_MUSH_RED:
		case B_TORCH:
		case B_FIRE:
		case B_SPAWNER:
		case B_REDSTONE_WIRE:
		case B_CROPS:
		case B_FARMLAND:
		case B_SNOW_LAYER:
		case B_ICE:
		case B_CACTUS:
		case B_REED:
		case B_PORTAL: return false;
		default: return true;
	}
}

// Light opacity != 0 (what stops Beta's heightMap, hence canBlockSeeTheSky).
// Informational use only.
static bool stops_sky(uint8_t id) {
	switch(id) {
		case B_AIR:
		case B_SAPLING:
		case B_GLASS:
		case B_TALLGRASS:
		case B_DEADBUSH:
		case B_DANDELION:
		case B_ROSE:
		case B_MUSH_BROWN:
		case B_MUSH_RED:
		case B_REED:
		case B_SNOW_LAYER:
		case B_TORCH:
		case B_FIRE:
		case B_CACTUS: return false;
		default: return true;
	}
}

// Every id a Beta 1.7.3 overworld populate pass may write
// [ChunkProviderGenerate.java:310-603 and the WorldGen* bodies it calls].
static bool populate_may_write(uint8_t id) {
	switch(id) {
		case B_AIR:		   // lakes (upper half), dungeon interior
		case B_STONE:	   // lava-lake rim
		case B_GRASS:	   // lake dirt->grass
		case B_DIRT:	   // dirt veins, tree base
		case B_COBBLE:	   // dungeon walls
		case B_WATER_FLOW: // water spring
		case B_WATER:	   // water lake
		case B_LAVA_FLOW:  // lava spring
		case B_LAVA:	   // lava lake
		case B_GRAVEL:
		case B_GOLD_ORE:
		case B_IRON_ORE:
		case B_COAL_ORE:
		case B_LOG:
		case B_LEAVES:
		case B_LAPIS_ORE:
		case B_TALLGRASS:
		case B_DEADBUSH:
		case B_DANDELION:
		case B_ROSE:
		case B_MUSH_BROWN:
		case B_MUSH_RED:
		case B_MOSSY:
		case B_SPAWNER:
		case B_CHEST:
		case B_DIAMOND_ORE:
		case B_REDSTONE_ORE:
		case B_SNOW_LAYER:
		case B_CACTUS:
		case B_CLAY:
		case B_REED:
		case B_PUMPKIN: return true;
		default: return false;
	}
}

// ---------------------------------------------------------------- clauses --
enum clause {
	C_REED_ON_SAND, // strict since G4 (reed-0926 a3c02638 landed)
	C_REED,
	C_CACTUS,
	C_FLOWER,
	C_TALLGRASS,
	C_DEADBUSH,
	C_MUSHROOM,
	C_PUMPKIN,
	C_SNOW,
	C_ORE,
	C_VEIN_FILLER, // dirt/gravel veins + tree-base dirt
	C_CLAY,
	C_LOG,
	C_LEAVES,
	C_SPRING,
	C_LAKE_GRASS,
	C_LAKE_RIM,
	C_UNNATURAL_ID,
	C_WINDOW, // offset plant/spring/snow features stay in their chunk window
	C_COUNT
};
static const char* const clause_name[C_COUNT] = {
	"REED_ON_SAND  BlockReed.java:33-36 (sand below; reed-0926 a3c02638)",
	"REED  BlockReed.java:33-36 + WorldGenReed.java:11-16",
	"CACTUS  BlockCactus.java:68-80 + WorldGenCactus.java:11-16",
	"FLOWER  BlockFlower.java:18-19,39-40 + WorldGenFlowers.java:17 (soil)",
	"TALLGRASS  BlockFlower.java:18-19 + WorldGenTallGrass.java:24-25",
	"DEADBUSH  BlockDeadBush.java:12-13 + WorldGenDeadBush.java:22",
	"MUSHROOM  BlockMushroom.java:29-34 + WorldGenFlowers.java:17 (soil+y)",
	"PUMPKIN  WorldGenPumpkin.java:11-12 + BlockPumpkin.java:36-38",
	"SNOW  ChunkProviderGenerate.java:594-597 + World.java:1114-1127",
	"ORE  WorldGenMinable.java:15-46 + ChunkProviderGenerate.java:368-408",
	"VEIN_FILLER  WorldGenMinable.java:45 dirt/gravel + WorldGenTrees:43",
	"CLAY  WorldGenClay.java:14-48",
	"LOG  WorldGenTrees.java:62-66 / WorldGenBigTree.java:194",
	"LEAVES  WorldGenTrees.java:55 / WorldGenBigTree.java:90-119",
	"SPRING  WorldGenLiquids.java:13-58",
	"LAKE_GRASS  WorldGenLakes (dirt->grass only)",
	"LAKE_RIM  WorldGenLakes lava rim (solid material only)",
	"UNNATURAL_ID  populate writes only the Beta populate id set",
	"WINDOW  ChunkProviderGenerate.java:473-597 seed +8 and feature spread",
};

struct clause_stat {
	long checked;
	long violations;
	int printed;
};
static struct clause_stat g_stat[C_COUNT];

// Informational (unasserted) light-term counts.
static long g_info_flower_no_sky, g_info_flower_total;
static long g_info_mush_sky, g_info_mush_total;
static long g_info_log_foreign_ground;

// ---------------------------------------------------------------- region --
#define R 8
#define IDX(x, y, z)                                                           \
	((size_t)(y) + ((size_t)(z) + (size_t)(x) * CHUNK_SIZE) * WORLD_HEIGHT)

static struct server_chunk g_ch[R][R];
static int g_cx0, g_cz0;
static int64_t g_seed;
static int g_px, g_pz; // chunk currently being populated

static struct server_chunk* reg_chunk(int x, int z, int* lx, int* lz) {
	int cx = (x >> 4) - g_cx0, cz = (z >> 4) - g_cz0;
	if(cx < 0 || cz < 0 || cx >= R || cz >= R)
		return NULL;
	*lx = x & 15;
	*lz = z & 15;
	return &g_ch[cx][cz];
}
static uint8_t reg_get(void* ctx, int x, int y, int z) {
	(void)ctx;
	int lx, lz;
	if(y < 0 || y >= WORLD_HEIGHT)
		return B_AIR;
	struct server_chunk* s = reg_chunk(x, z, &lx, &lz);
	return s ? s->ids[IDX(lx, y, lz)] : B_AIR;
}
static uint8_t reg_get_meta(void* ctx, int x, int y, int z) {
	(void)ctx;
	int lx, lz;
	if(y < 0 || y >= WORLD_HEIGHT)
		return 0;
	struct server_chunk* s = reg_chunk(x, z, &lx, &lz);
	return s ? nibble_read(s->metadata, IDX(lx, y, lz)) : 0;
}
static int reg_height(void* ctx, int x, int z) {
	(void)ctx;
	int lx, lz;
	struct server_chunk* s = reg_chunk(x, z, &lx, &lz);
	if(!s)
		return 0;
	for(int y = WORLD_HEIGHT - 1; y > 0; y--)
		if(s->ids[IDX(lx, y, lz)] != B_AIR)
			return y + 1;
	return 0;
}
static int reg_skylight(void* ctx, int x, int y, int z) {
	(void)ctx;
	int lx, lz;
	if(y >= WORLD_HEIGHT)
		return 15;
	if(y < 0)
		return 0;
	struct server_chunk* s = reg_chunk(x, z, &lx, &lz);
	return s ? nibble_read(s->lighting_sky, IDX(lx, y, lz)) : 0;
}

static bool sees_sky(int x, int y, int z) {
	for(int yy = y + 1; yy < WORLD_HEIGHT; yy++)
		if(stops_sky(reg_get(NULL, x, yy, z)))
			return false;
	return true;
}

static void violate(enum clause c, int x, int y, int z, uint8_t id,
					uint8_t prior, const char* why) {
	g_stat[c].violations++;
	if(g_stat[c].printed < 8) {
		g_stat[c].printed++;
		printf("  VIOLATION %.12s seed=%lld pos=(%d,%d,%d) id=%u prior=%u "
			   "below=%u populate=(%d,%d): %s\n",
			   clause_name[c], (long long)g_seed, x, y, z, id, prior,
			   reg_get(NULL, x, y - 1, z), g_px, g_pz, why);
	}
}
static void check(enum clause c, bool ok, int x, int y, int z, uint8_t id,
				  uint8_t prior, const char* why) {
	g_stat[c].checked++;
	if(!ok)
		violate(c, x, y, z, id, prior, why);
}

static bool water_beside(int x, int y, int z) {
	return mat_water(reg_get(NULL, x - 1, y, z))
		|| mat_water(reg_get(NULL, x + 1, y, z))
		|| mat_water(reg_get(NULL, x, y, z - 1))
		|| mat_water(reg_get(NULL, x, y, z + 1));
}

// Chunk-window checks: every decorate feature of chunk (px,pz) is seeded at
// bx + nextInt(16) (+8 for the offset features) and spreads by a fixed amount,
// so a write outside that window means a wrong offset. Strict bounds.
static bool in_window(int x, int z, double lo, double hi) {
	double dx = x - g_px * 16, dz = z - g_pz * 16;
	return dx > lo && dx < hi && dz > lo && dz < hi;
}
// WorldGenMinable/WorldGenClay seeded at bx+[0,15]: tube centre i+8 +- size/8
// (WorldGenMinable.java:16-19), half-width < size/16+0.5 (:27-28,:38-39).
static bool vein_in_window(int x, int z, int size) {
	double r = size / 8.0 + size / 16.0 + 0.5;
	return in_window(x, z, 8 - r - 1, 15 + 8 + r);
}
// Offset features: bx + nextInt(16) + 8 +- (spread-1).
static bool feature_in_window(int x, int z, int spread) {
	return in_window(x, z, 8 - spread, 15 + 8 + spread);
}

// The Beta rule for one populate write, on the PRE-write world.
static void check_write(int x, int y, int z, uint8_t id, uint8_t meta) {
	uint8_t prior = reg_get(NULL, x, y, z);
	uint8_t below = reg_get(NULL, x, y - 1, z);

	if(!populate_may_write(id)) {
		check(C_UNNATURAL_ID, false, x, y, z, id, prior,
			  "id outside the Beta populate write set");
		return;
	}
	g_stat[C_UNNATURAL_ID].checked++;

	// Offset-feature window: seed bx+nextInt(16)+8, spread nextInt(N)-nextInt(N).
	int spread = 0;
	switch(id) {
		case B_REED: spread = 4; break; // WorldGenReed.java:8-10
		case B_DANDELION:
		case B_ROSE:
		case B_MUSH_BROWN:
		case B_MUSH_RED:
		case B_TALLGRASS:
		case B_DEADBUSH:
		case B_PUMPKIN:
		case B_CACTUS: spread = 8; break; // nextInt(8)-nextInt(8)
		case B_WATER_FLOW:
		case B_LAVA_FLOW:
		case B_SNOW_LAYER: spread = 1; break; // exact column, CPG:574-597
		default: break;
	}
	if(spread)
		check(C_WINDOW, feature_in_window(x, z, spread), x, y, z, id, prior,
			  "feature write outside its populate chunk window");

	switch(id) {
		case B_REED: {
			// canPlaceBlockAt: reed below, OR grass/dirt below with water
			// material beside the BASE block (x+-1,y-1,z / x,y-1,z+-1).
			bool ok = prior == B_AIR
				&& (below == B_REED
					|| ((below == B_GRASS || below == B_DIRT)
						&& water_beside(x, y - 1, z)));
			if(!ok && below == B_SAND && prior == B_AIR) {
				check(C_REED_ON_SAND, false, x, y, z, id, prior,
					  "reed placed on sand (Beta: reed|grass|dirt only)");
			} else {
				g_stat[C_REED_ON_SAND].checked++;
				check(C_REED, ok, x, y, z, id, prior,
					  "reed without reed/grass/dirt+water support");
			}
			break;
		}
		case B_CACTUS: {
			bool sides = !mat_solid(reg_get(NULL, x - 1, y, z))
				&& !mat_solid(reg_get(NULL, x + 1, y, z))
				&& !mat_solid(reg_get(NULL, x, y, z - 1))
				&& !mat_solid(reg_get(NULL, x, y, z + 1));
			check(C_CACTUS,
				  prior == B_AIR && sides
					  && (below == B_CACTUS || below == B_SAND),
				  x, y, z, id, prior,
				  "cactus with a solid side or not on sand/cactus");
			break;
		}
		case B_DANDELION:
		case B_ROSE:
			check(C_FLOWER,
				  prior == B_AIR
					  && (below == B_GRASS || below == B_DIRT
						  || below == B_FARMLAND),
				  x, y, z, id, prior, "flower not on grass/dirt/farmland");
			g_info_flower_total++;
			if(!sees_sky(x, y, z))
				g_info_flower_no_sky++;
			break;
		case B_TALLGRASS:
			check(C_TALLGRASS,
				  prior == B_AIR
					  && (below == B_GRASS || below == B_DIRT
						  || below == B_FARMLAND)
					  && (meta == 1 || meta == 2),
				  x, y, z, id, prior,
				  "tall grass not on grass/dirt or meta not 1/2");
			break;
		case B_DEADBUSH:
			check(C_DEADBUSH, prior == B_AIR && below == B_SAND, x, y, z, id,
				  prior, "dead bush not on sand");
			break;
		case B_MUSH_BROWN:
		case B_MUSH_RED:
			check(C_MUSHROOM,
				  prior == B_AIR && y >= 0 && y < 128
					  && beta_opaque_cube(below),
				  x, y, z, id, prior,
				  "mushroom on a block Beta's opaqueCubeLookup rejects");
			g_info_mush_total++;
			if(sees_sky(x, y, z))
				g_info_mush_sky++;
			break;
		case B_PUMPKIN:
			check(C_PUMPKIN, prior == B_AIR && below == B_GRASS && meta < 4,
				  x, y, z, id, prior, "pumpkin not over grass / meta>=4");
			break;
		case B_SNOW_LAYER: {
			// findTopSolidBlock: first solid-or-liquid block from y=127 down,
			// +1. Snow goes at that y only if it is air and the block below
			// is solid material and not ice.
			bool top = true;
			for(int yy = y; yy < WORLD_HEIGHT; yy++) {
				uint8_t b = reg_get(NULL, x, yy, z);
				if(mat_solid(b) || mat_liquid(b))
					top = false;
			}
			check(C_SNOW,
				  prior == B_AIR && top && mat_solid(below) && below != B_ICE,
				  x, y, z, id, prior,
				  "snow not on the top solid block / below not solid / on ice");
			break;
		}
		case B_COAL_ORE:
		case B_IRON_ORE:
		case B_GOLD_ORE:
		case B_REDSTONE_ORE:
		case B_DIAMOND_ORE:
		case B_LAPIS_ORE: {
			// WorldGenMinable.java:20-21 puts both tube endpoints at j+2..j+4
			// and :27-29,:41-42 give a half-height < size/16+0.5, so a block
			// lands at 1 <= y <= j+4 for every ore size (<=16). Origin j
			// ranges: ChunkProviderGenerate.java:368-408 (coal 128, iron 64,
			// gold 32, redstone/diamond 16, lapis 16+16).
			int ymax = 127 + 4;
			int size = 7;
			switch(id) {
				case B_COAL_ORE: size = 16; break;
				case B_IRON_ORE:
					ymax = 63 + 4;
					size = 8;
					break;
				case B_GOLD_ORE:
					ymax = 31 + 4;
					size = 8;
					break;
				case B_REDSTONE_ORE:
				case B_DIAMOND_ORE: ymax = 15 + 4; break;
				case B_LAPIS_ORE:
					ymax = 30 + 4;
					size = 6;
					break;
				default: break;
			}
			check(C_ORE,
				  prior == B_STONE && y >= 1 && y <= ymax
					  && vein_in_window(x, z, size),
				  x, y, z, id, prior,
				  "ore over non-stone, outside its y range or chunk window");
			break;
		}
		case B_GRAVEL:
			check(C_VEIN_FILLER, prior == B_STONE && vein_in_window(x, z, 32),
				  x, y, z, id, prior,
				  "gravel vein over non-stone or outside its chunk window");
			break;
		case B_DIRT:
			// Dirt veins replace stone; WorldGenTrees/Forest/Taiga turn the
			// grass/dirt under the trunk into dirt.
			check(C_VEIN_FILLER,
				  prior == B_STONE || prior == B_GRASS || prior == B_DIRT, x,
				  y, z, id, prior, "dirt written over a non-stone/non-soil");
			break;
		case B_CLAY:
			check(C_CLAY, prior == B_SAND && vein_in_window(x, z, 32), x, y, z,
				  id, prior, "clay over non-sand or outside its chunk window");
			break;
		case B_LOG:
			// Oak/birch/spruce trunks write only over air|leaves
			// (WorldGenTrees.java:62-66, Forest/Taiga1/Taiga2 alike);
			// WorldGenBigTree draws trunk + limbs with an UNCONDITIONAL
			// setBlock line (WorldGenBigTree.java:194, func_522_a) after its
			// path checks, so limbs legitimately rewrite their own trunk logs.
			check(C_LOG, prior == B_AIR || prior == B_LEAVES || prior == B_LOG,
				  x, y, z, id, prior,
				  "log overwrote something other than air/leaves/log");
			if(below != B_LOG && below != B_LEAVES && below != B_AIR
			   && below != B_GRASS && below != B_DIRT)
				g_info_log_foreign_ground++;
			break;
		case B_LEAVES:
			// Trees replace non-opaque cells only; the leaf-removal |8 pass
			// rewrites leaves over leaves.
			check(C_LEAVES, prior == B_LEAVES || !beta_opaque_cube(prior), x,
				  y, z, id, prior, "leaves overwrote an opaque block");
			break;
		case B_WATER_FLOW:
		case B_LAVA_FLOW: {
			int stone = 0, air = 0;
			const uint8_t n[4] = {
				reg_get(NULL, x - 1, y, z), reg_get(NULL, x + 1, y, z),
				reg_get(NULL, x, y, z - 1), reg_get(NULL, x, y, z + 1)};
			for(int k = 0; k < 4; k++) {
				stone += n[k] == B_STONE;
				air += n[k] == B_AIR;
			}
			check(C_SPRING,
				  reg_get(NULL, x, y + 1, z) == B_STONE && below == B_STONE
					  && (prior == B_AIR || prior == B_STONE) && stone == 3
					  && air == 1,
				  x, y, z, id, prior, "spring outside a 3-stone/1-air pocket");
			break;
		}
		case B_GRASS:
			check(C_LAKE_GRASS, prior == B_DIRT, x, y, z, id, prior,
				  "grass written over a non-dirt block");
			break;
		case B_STONE:
			check(C_LAKE_RIM, mat_solid(prior), x, y, z, id, prior,
				  "lava-lake rim stone over a non-solid block");
			break;
		default: break; // air / still fluids / dungeon blocks: no local rule
	}
}

static void reg_set(void* ctx, int x, int y, int z, uint8_t id, uint8_t m) {
	(void)ctx;
	int lx, lz;
	if(y < 0 || y >= WORLD_HEIGHT)
		return;
	struct server_chunk* s = reg_chunk(x, z, &lx, &lz);
	if(!s)
		return;
	check_write(x, y, z, id, m);
	uint8_t old = s->ids[IDX(lx, y, lz)];
	s->ids[IDX(lx, y, lz)] = id;
	nibble_write(s->metadata, IDX(lx, y, lz), m);
	// Mirror the live quad writer (server_world.c quad_set): Beta's
	// BlockLeaves.onBlockRemoval check-decay flag pass.
	if(old == B_LEAVES && id != B_LEAVES) {
		struct wgen_world w = {0};
		w.get = reg_get;
		w.set = reg_set;
		w.get_meta = reg_get_meta;
		wgen_leaves_on_removal(&w, g_px, g_pz, x, y, z);
	}
}

// ------------------------------------------------------------------ sweep --
static long g_populated;

static bool run_region(struct worldgen* wg, int64_t seed, int cx0, int cz0,
					   const char* label) {
	g_seed = seed;
	g_cx0 = cx0;
	g_cz0 = cz0;
	for(int i = 0; i < R; i++)
		for(int j = 0; j < R; j++)
			if(!worldgen_generate_chunk(wg, &g_ch[i][j], cx0 + i, cz0 + j)) {
				printf("FAIL: worldgen_generate_chunk(%d,%d) seed %lld\n",
					   cx0 + i, cz0 + j, (long long)seed);
				return false;
			}
	struct wgen_world w = {0};
	w.get = reg_get;
	w.set = reg_set;
	w.height = reg_height;
	w.skylight = reg_skylight;
	w.get_meta = reg_get_meta;
	for(int i = 0; i < R - 1; i++)
		for(int j = 0; j < R - 1; j++) {
			g_px = cx0 + i;
			g_pz = cz0 + j;
			enum biome b
				= worldgen_biome_at(wg, g_px * 16 + 16, g_pz * 16 + 16);
			double td = worldgen_tree_density(wg, g_px * 16, g_pz * 16);
			worldgen_populate(seed, g_px, g_pz, &w, b, td);
			double temp[256];
			worldgen_temperature_grid(wg, g_px * 16 + 8, g_pz * 16 + 8, temp);
			worldgen_snow(&w, temp, g_px * 16, g_pz * 16);
			g_populated++;
		}
	for(int i = 0; i < R; i++)
		for(int j = 0; j < R; j++)
			server_chunk_arrays_free(&g_ch[i][j]);
	printf("  region seed=%lld chunks (%d..%d, %d..%d) %s\n", (long long)seed,
		   cx0, cx0 + R - 1, cz0, cz0 + R - 1, label);
	return true;
}

// Nearest chunk (spiral by Chebyshev ring) whose populate corner biome is one
// of `want`; returns false if none within `radius`.
static bool find_biome(struct worldgen* wg, const enum biome* want, int nwant,
					   int radius, int* ocx, int* ocz) {
	for(int r = 0; r <= radius; r++)
		for(int dx = -r; dx <= r; dx++)
			for(int dz = -r; dz <= r; dz++) {
				if(abs(dx) != r && abs(dz) != r)
					continue;
				enum biome b = worldgen_biome_at(wg, dx * 16 + 16, dz * 16 + 16);
				for(int k = 0; k < nwant; k++)
					if(b == want[k]) {
						*ocx = dx;
						*ocz = dz;
						return true;
					}
			}
	return false;
}

int main(void) {
	// 12345 + 42 cover the two known reed-on-sand sites (REED-SAND-DIAG-0926);
	// 5 / 37 / 41 are the first three seeds of tests/seed_corpus.tsv.
	struct {
		int64_t seed;
		int cx0, cz0; // fixed region origin
	} seeds[] = {
		{12345, 3, -9}, // reed (119,71,-78) in chunk (7,-5)
		{42, -5, -2},	// reed (-16,64,43) in chunk (-1,2)
		{5, -4, -4},
		{37, -4, -4},
		{41, -4, -4},
		/* WIP 356da47fe corrected Beta's tree-density Perlin axis
		 * (CFR NoiseGeneratorPerlin.java:91-92). The resulting decorator
		 * RNG stream needs another region to exercise pumpkin placement. */
		{2, -4, -4},
	};
	// One extra region per biome group, so every biome-gated decorator
	// (cactus / dead bush: desert; snow: cold; pumpkins + flowers: grassland;
	// every tree generator) is exercised.
	static const enum biome g_desert[] = {BIOME_DESERT};
	static const enum biome g_cold[] = {BIOME_TAIGA, BIOME_TUNDRA};
	static const enum biome g_plains[] = {BIOME_PLAINS, BIOME_SAVANNA,
										  BIOME_SHRUBLAND};
	static const enum biome g_forest[] = {BIOME_FOREST, BIOME_SEASONAL_FOREST};
	static const enum biome g_wet[] = {BIOME_RAINFOREST, BIOME_SWAMPLAND};
	const struct {
		const enum biome* want;
		int n;
		const char* label;
	} groups[] = {
		{g_desert, 1, "desert"},
		{g_cold, 2, "taiga/tundra"},
		{g_plains, 3, "plains/savanna/shrubland"},
		{g_forest, 2, "forest/seasonal"},
		{g_wet, 2, "rainforest/swamp"},
	};
	int regions = 0;

	for(size_t s = 0; s < sizeof seeds / sizeof seeds[0]; s++) {
		struct worldgen* wg
			= worldgen_create(seeds[s].seed, WORLD_DIM_OVERWORLD);
		if(!wg) {
			printf("FAIL: worldgen_create\n");
			return 1;
		}
		if(!run_region(wg, seeds[s].seed, seeds[s].cx0, seeds[s].cz0,
					   "fixed"))
			return 1;
		regions++;
		for(size_t gi = 0; gi < sizeof groups / sizeof groups[0]; gi++) {
			int cx, cz;
			if(!find_biome(wg, groups[gi].want, groups[gi].n, 64, &cx, &cz))
				continue;
			if(!run_region(wg, seeds[s].seed, cx - R / 2, cz - R / 2,
						   groups[gi].label))
				return 1;
			regions++;
		}
		worldgen_destroy(wg);
	}

	printf("\n%d regions, %ld chunk populates\n", regions, g_populated);
	printf("%-72s %9s %6s\n", "clause", "checked", "viol");
	int hard_fail = 0;
	for(int c = 0; c < C_COUNT; c++) {
		printf("%-72s %9ld %6ld\n", clause_name[c], g_stat[c].checked,
			   g_stat[c].violations);
		if(g_stat[c].violations != 0)
			hard_fail = 1;
	}
	// Vacuity guard: every placement clause must have been exercised.
	for(int c = C_REED; c < C_COUNT; c++)
		if(c != C_UNNATURAL_ID && g_stat[c].checked == 0) {
			printf("FAIL: clause never exercised (sweep too small): %s\n",
				   clause_name[c]);
			hard_fail = 1;
		}

	printf("\ninfo (unasserted, time-of-day dependent in Beta): flowers placed "
		   "without sky view %ld/%ld; mushrooms placed with sky view %ld/%ld; "
		   "logs resting on non-soil %ld\n",
		   g_info_flower_no_sky, g_info_flower_total, g_info_mush_sky,
		   g_info_mush_total, g_info_log_foreign_ground);

	if(hard_fail) {
		printf("FAIL: decoration placement violates Beta 1.7.3 rules\n");
		return 1;
	}
	printf("OK: every populate write obeys its Beta 1.7.3 placement rule\n");
	return 0;
}
