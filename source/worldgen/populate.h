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

#ifndef WORLDGEN_POPULATE_H
#define WORLDGEN_POPULATE_H

#include <stdbool.h>
#include <stdint.h>

#include "biome.h"

/*
	The Beta populate (decorate) phase writes ACROSS chunk borders: the +8
	placement offset deliberately spills ore veins and trees into the +X/+Z
	neighbour chunks, which is why Beta defers a chunk's population until its
	+X, +Z and +X+Z neighbours all exist. Every decoration feature therefore
	operates through this world-view abstraction, which reads and writes a
	block at WORLD coordinates rather than chunk-local ones:

	  - during real generation it is backed by the 2x2 chunk quadrant being
	    populated (all four chunks are guaranteed loaded);
	  - in unit tests it is backed by a flat buffer.

	get()      returns the block id at (x,y,z); it MUST return BLOCK_AIR (0) for
	           out-of-world coordinates (e.g. y<0 or y>=128) so the stone/air
	           checks behave exactly like Beta's World.getBlockId out of bounds.
	set()      writes id+metadata, silently ignoring out-of-world coordinates.
	height()   returns the world surface height at column (x,z). NOT read by
	           populate: trees compute Beta's World.getHeightValue from get()
	           over the current blocks (tree.c tree_height_value), because
	           Beta's heightMap follows every populate write and the live
	           quadrant's hook (server_world.c quad_height) is the terrain-time
	           heightmap no populate write updates (laneC/TREE-HEIGHTMAP).
	           Kept only for existing world-view literals; do not add readers.
	skylight() returns the saved sky-light level [0..15] at (x,y,z); MUST return
	           15 above the world (y>=128, full daylight) and 0 below (y<0), to
	           match Beta's World.getSavedLightValue out of bounds. Used by the
	           lake generator to convert exposed dirt to grass only where the
	           column sees sky.

	A feature that does not need the heightmap may leave height == NULL; one
	that does not need lighting may leave skylight == NULL.
*/
struct item_data; // item/items.h -- one chest loot slot (id 0 == empty)
struct java_random;

// Slots a single (dungeon) chest exposes to loot, matching Beta's
// TileEntityChest.getSizeInventory(); the loot slot index is nextInt(this).
#define DUNGEON_CHEST_SLOTS 27

struct wgen_world {
	void* ctx;
	uint8_t (*get)(void* ctx, int x, int y, int z);
	void (*set)(void* ctx, int x, int y, int z, uint8_t id, uint8_t metadata);
	int (*height)(void* ctx, int x, int z);
	int (*skylight)(void* ctx, int x, int y, int z);
	// Stock a freshly generated container (a dungeon chest at x,y,z) with its
	// rolled loot: `items` points at a DUNGEON_CHEST_SLOTS-element array (slot
	// -> item, id 0 == empty). NULL when the world-view has no container store
	// (unit tests, find_* tools); worldgen still consumes the loot RNG either
	// way, so a NULL hook skips only the materialisation, never the stream.
	void (*set_chest)(void* ctx, int x, int y, int z,
					  const struct item_data* items);
	// WorldGenLiquids / WorldGenHellLava place the moving-fluid source, then set
	// scheduledUpdatesAreImmediate and invoke its updateTick before returning to
	// the next decoration. Optional for pure/offline world views; the live
	// quadrant supplies a raw (no CRPC/lighting) immediate fluid adapter.
	void (*tick_fluid_immediate)(void* ctx, struct java_random* feature_rng,
								 int x, int y, int z, uint8_t type);
	// FH-17: materialise a freshly generated dungeon mob spawner's Beta
	// EntityId (a MobSpawner tile entity) at (x,y,z): `mob_id` is a static
	// literal ("Skeleton"/"Zombie"/"Spider"), valid for the whole populate
	// call. NULL when the world-view has no spawner store (unit tests/tools);
	// worldgen still draws the pickMobSpawner RNG either way, so a NULL hook
	// skips only the materialisation, never the stream -- exactly set_chest's
	// contract above. Appended at the END of this struct (not alongside
	// set_chest) so existing positional-braced wgen_world literals
	// (tests/test_dungeon*.c) keep compiling with this field defaulting NULL.
	void (*set_spawner)(void* ctx, int x, int y, int z, const char* mob_id);
	// next-gaps-0924/W1E-c: metadata nibble read (World.getBlockMetadata) for
	// the populate-time leaf-removal flag pass (wgen_leaves_on_removal below).
	// NULL on a world-view without metadata storage (such a writer does not
	// call the pass). Appended at the END so positional wgen_world literals
	// keep compiling with it NULL.
	uint8_t (*get_meta)(void* ctx, int x, int y, int z);
	/* World.setBlock[WithNotify] -> Chunk.setBlockID; distinct from set's
	 * id+metadata API. Optional for buffer-only worlds with no lighting state. */
	void (*set_plain)(void* ctx, int x, int y, int z, uint8_t id);
	/* World.getFullBlockLightValue: actual saved Sky/Block at this phase.
	 * A buffer-only Nether fixture without light storage starts unlit. */
	int (*full_light)(void* ctx, int x, int y, int z);
	/* CFR BlockLeaves.java:48: metadata-only removal callback stores. */
	void (*set_meta)(void* ctx, int x, int y, int z, uint8_t metadata);
	/* Actual provider residency, not the guaranteed minimum populate window. */
	bool (*chunks_exist)(void* ctx, int x1, int y1, int z1, int x2, int y2, int z2);
	void (*set_plain_notify)(void* ctx, int x, int y, int z, uint8_t id);
};


/* CFR WorldGenMinable:59, WorldGenLakes:79-98, WorldGenTrees:53-69:
 * preserve plain-ID API kind; buffer-only views use their equivalent writer. */
static inline void wgen_set_plain(struct wgen_world* w, int x, int y, int z,
                                 uint8_t id) {
	if(w->set_plain) w->set_plain(w->ctx, x, y, z, id);
	/* Buffer-only metadata writers must retain Chunk.setBlockID's no-op,
	 * including species/decay bits on an unchanged leaf ID (CFR Chunk:245). */
	else if(w->get(w->ctx, x, y, z) != id) w->set(w->ctx, x, y, z, id, 0);
}


/* CFR WorldGenDungeons.java:57-102 / WorldGenGlowStone1.java:26,57. */
static inline void wgen_set_plain_notify(struct wgen_world* w, int x, int y,
                                       int z, uint8_t id) {
	if(w->set_plain_notify) w->set_plain_notify(w->ctx, x, y, z, id);
	else wgen_set_plain(w, x, y, z, id);
}

/*
	Beta populate() writes go through World.setBlock[AndMetadata] ->
	Chunk.setBlockID [740c583 Chunk.java:277-286] / setBlockIDWithMetadata
	[:240-248], which invoke the OLD block's onBlockRemoval whenever the id
	changes. For LEAVES that is BlockLeaves.onBlockRemoval [BlockLeaves.java:
	33-50]: every leaf in the 3x3x3 around the replaced cell gets metadata |8
	(the check-decay flag), gated on World.checkChunksExist at radius 2
	[World.java:327-348]. Trees do this to themselves: WorldGenTrees places its
	canopy first [:46-59, the trunk column included] and then overwrites the
	column's canopy cells with wood [:62-66] (WorldGenForest:46-65,
	WorldGenTaiga1:55-70, WorldGenTaiga2:60-82, WorldGenBigTree:119/194
	likewise), so real Beta trees generate with bit 8 on the leaves around the
	trunk top. Lakes/dungeons/springs replacing a neighbouring chunk's earlier
	leaves fire it too.

	wgen_leaves_on_removal() is that onBlockRemoval for a populate writer. The
	WRITER calls it right after storing a new id when the byte it replaced was
	LEAVES and the new id is not (the quad writer does this on its own one-byte
	old-id load, server_world.c quad_set; test fixtures mirror it). The centre
	already reads as the new id, so it is never re-flagged; each 3x3x3 leaf is
	rewritten as LEAVES with meta | 8 through get_meta/set (World.
	setBlockMetadata). Chunk existence during populate is the 2x2 window
	(chunk_x..+1, chunk_z..+1): the only chunks Beta GUARANTEES at populate
	time (ChunkProviderLoadOrGenerate.loadChunk:76-90); any other chunk's
	existence there is load-order dependent in Beta itself, and the quad writer
	drops writes outside the window, so "does not exist" is the one
	self-consistent model. A writer without get_meta must not call it (nothing
	to preserve the species bits with); the block writes and the RNG stream are
	never involved either way.

	Same-id case, UNREACHABLE in Beta: setBlockIDWithMetadata returns early only
	when id AND metadata match [Chunk.java:240], so leaves(1|2) written over a
	leaf of another species WOULD fire onBlockRemoval [:246-247] -- but no Beta
	generator ever writes a leaf over a leaf: opaqueCubeLookup[18] is TRUE
	(Block.java:151 calls BlockLeaves.isOpaqueCube = !graphicsLevel before
	BlockLeavesBase assigns graphicsLevel, and RenderGlobal.java:188 never
	touches the static table) and WorldGenTrees:55 / WorldGenForest:55 /
	WorldGenTaiga1:54 / WorldGenTaiga2:59 skip opaque cells. CaveWii's
	tree_is_opaque (tree.c) treats leaves the same way, so the case never
	arises here either.
*/
void wgen_leaves_on_removal(struct wgen_world* w, int chunk_x, int chunk_z,
							int x, int y, int z);
void wgen_log_on_removal(struct wgen_world* w, int chunk_x, int chunk_z,
	int x, int y, int z);

/*
	Beta 1.7.3 WorldGenMinable: carve one ore vein -- a line of `size`+1
	overlapping spheres swept between two endpoints -- replacing STONE only,
	seeded at world block (i,j,k). The vein is centred at (i+8, j, k+8), so a
	vein seeded in one chunk spills into its +X/+Z neighbours (hence the
	world-view abstraction).

	Byte-exact PRNG consumption order (load-bearing): nextFloat() for the
	angle, nextInt(3) and nextInt(3) for the two y endpoints, then exactly one
	nextDouble() per sweep step l in [0, size]. The block writes consume no
	randomness.
*/
void worldgen_minable(struct wgen_world* w, struct java_random* rand, int i,
					  int j, int k, int size, uint8_t ore_id, uint8_t ore_meta);

/*
	Beta 1.7.3 ore-vein passes (one step of the full populate() sequence): the
	eight stone-replacing veins -- dirt, gravel, coal, iron, gold, redstone,
	diamond, lapis -- in Beta's exact decorate order, each driven by
	worldgen_minable. `block_x`/`block_z` are the chunk's block origin
	(chunk_x*16, chunk_z*16). The per-vein draw order is nextInt(16) [x],
	nextInt(yRange) [y], nextInt(16) [z], then the vein; lapis's y is two
	ordered draws. Caller supplies the per-chunk decorate Random, already
	advanced past the earlier decorate steps (lakes/dungeons/clay) so the vein
	positions match Beta.
*/
void worldgen_ores(struct wgen_world* w, struct java_random* rand, int block_x,
				   int block_z);

/*
	Beta 1.7.3 WorldGenLakes: carve a fluid pond at world block (x,y,z). Faithful
	to vanilla: subtract 8 from x and z, descend to the first non-air block, drop
	4, then OR together 4..7 ellipsoid blobs into a 16x16x8 mask. Validates the
	shell (aborts if the upper half (y>=4) touches existing liquid, or the lower
	half (y<4) touches a non-solid block that is not the fluid itself), then fills
	the mask (lower half = fluid, upper half = air), turns exposed dirt to grass
	where the column sees sky, and -- for lava -- rims the pool with stone.

	`fluid_id` is BLOCK_WATER_STILL or BLOCK_LAVA_STILL. Returns whether the lake
	generated (false = validation abort); the populate caller ignores it, as
	vanilla does. Byte-exact PRNG consumption (load-bearing): nextInt(4)+4 for the
	blob count, then 6 nextDouble() per blob (radii X/Y/Z then centres X/Y/Z); a
	lava lake additionally draws one nextInt(2) per shell-edge cell at y>=4. The
	validation/fill/grass passes consume no randomness. Needs w->skylight (the
	grass pass) -- a NULL skylight just skips the dirt->grass conversion.
*/
bool worldgen_lake(struct wgen_world* w, struct java_random* rand, int x, int y,
				   int z, uint8_t fluid_id);

/*
	Beta 1.7.3 WorldGenDungeons: try to carve a cobblestone dungeon centred at
	world block (px,py,pz). Draws two width values (nextInt(2)+2 each, consumed
	even on abort), aborts unless the box has a solid floor + ceiling and 1..5
	wall openings, then hollows a room wrapped in cobblestone (the floor is mossy
	3/4 of the time -- one nextInt(4) per solid floor block, terrain-dependent),
	places up to two loot chests (each: up to 3 position tries of 2 draws, then 8
	faithful loot rolls + a nextInt(27) slot per item) and a centre mob spawner
	(one nextInt(4) for the mob type). Returns whether it generated.

	Chest loot is now MATERIALISED through w->set_chest -- each rolled item is
	placed at its nextInt(27) slot, faithful to Beta's getDungeonLootItem (a
	later item overwrites an earlier slot, as setInventorySlotContents does). The
	sole exception is music discs (item id 2256/2257, beyond the fork's
	ITEMS_MAX): their slot is occupied but left empty, so the RNG and every other
	item stay byte-exact.

	FH-17: the spawner's mob type is MATERIALISED through w->set_spawner as a
	Beta MobSpawner tile entity (EntityId string), not the block's metadata
	nibble -- the metadata is always written 0, Beta's own faithful spawner-
	block value. The nextInt(4) draw itself is unchanged (same position in the
	decorate stream); only its destination moved.
*/
bool worldgen_dungeon(struct wgen_world* w, struct java_random* rand, int px,
					  int py, int pz);

/*
	Beta 1.7.3 WorldGenClay: a clay blob of `size` (32 in populate) seeded at
	world block (i,j,k). Generates ONLY when (i,j,k) is water -- the check is
	before any draw, so a dry seed consumes no randomness. Otherwise identical
	swept-blob geometry + PRNG order to worldgen_minable (nextFloat angle, two
	nextInt(3) y-endpoints, one nextDouble per step), replacing SAND with CLAY.
	Returns whether it generated.
*/
bool worldgen_clay(struct wgen_world* w, struct java_random* rand, int i, int j,
				   int k, int size);

/*
	Beta 1.7.3 populate() entry point. Sets up the per-chunk decorate Random
	(setSeed(seed); l1=nextLong/2*2+1; l2=nextLong/2*2+1;
	setSeed((cx*l1 + cz*l2) ^ seed)) and runs the decorate sequence over the
	chunk's 2x2 quadrant via `w`. `corner_biome` (the biome at the +16,+16
	corner) and `tree_density` (the treeDensity octave sample at the block
	origin) feed the tree step; the caller computes them from the generator so
	this stays generator-free.
*/
void worldgen_populate(int64_t seed, int chunk_x, int chunk_z,
					   struct wgen_world* w, enum biome corner_biome,
					   double tree_density);

#endif
