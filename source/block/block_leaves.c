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

#include "../graphics/render_block.h" // direct include (was transitive via blocks.h pre-split)
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_world.h"
#include "block_leaves.h"
#include "blocks.h"

/* See block_leaves_set_fancy (block_leaves.h). Default: Beta fast. */
static bool s_leaves_fancy = false;

static enum block_material getMaterial(struct block_info* this) {
	return MATERIAL_ORGANIC;
}

static size_t getBoundingBox(struct block_info* this, bool entity,
							 struct AABB* x) {
	if(x)
		aabb_setsize(x, 1.0F, 1.0F, 1.0F);
	return 1;
}

/*
struct face_occlusion sides_mask = {
	.mask = {0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA,
			 0xAAAAAAAA, 0xAAAAAAAA, 0xAAAAAAAA},
};
*/

static struct face_occlusion*
getSideMask(struct block_info* this, enum side side, struct block_info* it) {
/*	return (it->block->type == this->block->type) ?
		((side == SIDE_RIGHT || side == SIDE_BACK || side == SIDE_BOTTOM) ?
			 face_occlusion_full() :
			 &sides_mask) :
		&sides_mask;
*/
	/* Beta fancy: leaves are not an opaque cube and hide no face, so every
	 * leaf-to-leaf face is drawn [BlockLeavesBase.java:15-18]. Beta fast:
	 * isOpaqueCube() is true [BlockLeaves.java:173-175], so a leaf hides
	 * (and is hidden by) its neighbours exactly like a full opaque block. */
	return s_leaves_fancy ? face_occlusion_empty() : face_occlusion_full();
}

void block_leaves_set_fancy(bool fancy) {
	s_leaves_fancy = fancy;
}

bool block_leaves_fancy(void) {
	return s_leaves_fancy;
}

uint8_t block_leaves_raw_oak_texture(struct block_info* this, enum side side) {
	(void)this;
	(void)side;
	return tex_atlas_lookup(s_leaves_fancy ? TEXAT_LEAVES_OAK_RAW
										   : TEXAT_LEAVES_OAK_FAST_RAW);
}

static uint8_t getTextureIndex(struct block_info* this, enum side side) {
	/* BlockLeaves.getBlockTextureFromSideAndMetadata: (meta & 3) == 1 ->
	 * spruce (+80), else the base tile; the base is 52 fancy / 53 fast
	 * [BlockLeaves.java:177-184]. Birch shares the oak tile with its own
	 * colour (the _BIRCH atlas cell). */
	switch(this->block->metadata & 0x3) {
		default:
		case 0:
			return tex_atlas_lookup(s_leaves_fancy ? TEXAT_LEAVES_OAK
												   : TEXAT_LEAVES_OAK_FAST);
		case 1:
			return tex_atlas_lookup(s_leaves_fancy ? TEXAT_LEAVES_SPRUCE
												   : TEXAT_LEAVES_SPRUCE_FAST);
		case 2:
			return tex_atlas_lookup(s_leaves_fancy ? TEXAT_LEAVES_BIRCH
												   : TEXAT_LEAVES_BIRCH_FAST);
	}
}

static size_t getDroppedItem(struct block_info* this, struct item_data* it,
							 struct java_random* g, struct server_local* s) {
	/* BlockLeaves.idDropped:137 and damageDropped:151; quantity is separate. */
	if(it) {
		it->id = BLOCK_SAPLING;
		it->durability = this->block->metadata & 0x3;
		it->count = 1;
	}
	return 1;
}

/* ---- Beta leaf decay [740c583 BlockLeaves.java:33-149, BlockLog.java:23-42]
 *
 * Beta never scans for "a log somewhere nearby". A leaf only decays when
 * (1) it carries the check-decay flag (metadata bit 8), which is set by the
 *     removal of a neighbouring LEAF (radius 1) or LOG (radius 4), or by
 *     ItemLeaves placement (damage | 8), and
 * (2) its random tick finds no log reachable through a 6-connected chain of
 *     LEAF cells of length <= 4 (a bounded BFS over a 32^3 scratch array).
 * A flagged leaf that does reach a log is unflagged (setBlockMetadata, no
 * notify) and never rescanned until something is removed next to it again.
 * The old CaveWii rule ("no log in the 9x9x9 cube -> decay, even through
 * air; a log anywhere in it -> live") was wrong in both directions: placed
 * leaves decayed, detached clumps beside a trunk never did.
 *
 * WII: Beta lazily allocates `int[32*32*32]` (128 KB) once and NEVER clears
 * it between ticks. The static buffer below is that field: zero at process
 * start (Java zero-initialises the array), persistent, no per-tick malloc.
 * The stored values are -2..4, so int8_t is bit-equivalent. And of Beta's
 * 32 cells per axis only offsets -5..+5 are ever addressed: the region fill
 * writes -4..+4, a level pass reads -4..+4 and writes 6-neighbours, i.e.
 * -5..+5, and the verdict reads the centre. An offset-5 cell is written only
 * when it already holds -2, and nothing ever stores -2 there, so the outer
 * shell stays at its initial 0 forever -- it is never a source (a source
 * must lie in -4..+4) and never a sink. The 11^3 buffer below is therefore
 * bit-exact to the 32^3 field (T1; tests/test_leaf_decay_scratch_equiv.c
 * runs the 32^3 transcription and this port side by side, including the
 * stale-centre sequences): 1331 bytes of .bss instead of 32 KB.
 * Worst case per updateTick: 9^3 = 729 server_world_get_block calls in the
 * region fill (one per cell -- server_world's single-entry chunk cache
 * thrashes whenever the 9x9 footprint straddles a chunk border, so this is
 * up to 729 chunk-dict probes on the Wii), then 4 distance passes x 729
 * scratch reads, each hit writing at most 6 neighbours -- about 3.6K scratch
 * touches, zero allocations. The chunk gate before the fill is 11x11 = up to
 * 4 chunk-dict probes. */
#define LEAVES_SCRATCH_HALF 5 /* Beta's scratch half-size is 16; only |offset| <= 5 is live */
#define LEAVES_SCRATCH_SIDE (2 * LEAVES_SCRATCH_HALF + 1) /* Beta's scratch side is 32 */
#define LEAVES_SCRATCH_SQ (LEAVES_SCRATCH_SIDE * LEAVES_SCRATCH_SIDE) /* Beta's side squared */
#define LEAVES_WALK_RADIUS 4 /* Beta's walk radius */
#define LEAVES_SCRATCH_IDX(i, j, k)                                             \
	(((i) + LEAVES_SCRATCH_HALF) * LEAVES_SCRATCH_SQ                            \
	 + ((j) + LEAVES_SCRATCH_HALF) * LEAVES_SCRATCH_SIDE + (k)                  \
	 + LEAVES_SCRATCH_HALF)

static int8_t adjacentTreeBlocks[LEAVES_SCRATCH_SIDE * LEAVES_SCRATCH_SIDE
								 * LEAVES_SCRATCH_SIDE]; /* 11^3 = 1331 B */

#ifdef CAVEWII_TESTING
static uint32_t s_scan_lookups;
uint32_t block_leaves_test_scan_lookups(void) {
	return s_scan_lookups;
}
#endif

/* World.checkChunksExist [740c583 World.java:327-348]: every chunk column
 * touched by the block box must be loaded; a box entirely above/below the
 * world fails. Beta's `>> 4` on a negative coordinate is an arithmetic shift
 * (floor division), which WCOORD_CHUNK_OFFSET spells out portably. */
static bool checkChunksExist(struct server_world* w, w_coord_t x1,
							 w_coord_t y1, w_coord_t z1, w_coord_t x2,
							 w_coord_t y2, w_coord_t z2) {
	if(!(y2 >= 0 && y1 < WORLD_HEIGHT))
		return false;
	w_coord_t cx1 = WCOORD_CHUNK_OFFSET(x1), cx2 = WCOORD_CHUNK_OFFSET(x2);
	w_coord_t cz1 = WCOORD_CHUNK_OFFSET(z1), cz2 = WCOORD_CHUNK_OFFSET(z2);
	for(w_coord_t cx = cx1; cx <= cx2; cx++)
		for(w_coord_t cz = cz1; cz <= cz2; cz++)
			if(!server_world_is_chunk_loaded(w, cx, cz))
				return false;
	return true;
}

/* The shared body of BlockLeaves.onBlockRemoval (radius 1, flags every leaf
 * unconditionally -- Java re-writes an already-set nibble) and
 * BlockLog.onBlockRemoval (radius 4, `(meta & 8) == 0` guard). Both gate on
 * checkChunksExist at radius + 1 and walk x outer, y, z inner. The flag write
 * is World.setBlockMetadata: nibble only, no lifecycle/light/notify. */
static void flagLeavesForDecay(struct server_local* s, w_coord_t x,
							   w_coord_t y, w_coord_t z, int radius,
							   bool skip_flagged) {
	const int reach = radius + 1;
	if(!checkChunksExist(block_world(s), x - reach, y - reach, z - reach, x + reach,
						 y + reach, z + reach))
		return;
	for(int dx = -radius; dx <= radius; dx++) {
		for(int dy = -radius; dy <= radius; dy++) {
			for(int dz = -radius; dz <= radius; dz++) {
				struct block_data b;
				if(!server_world_get_block(block_world(s), x + dx, y + dy, z + dz,
										   &b)
				   || b.type != BLOCK_LEAVES)
					continue;
				if(skip_flagged && (b.metadata & BLOCK_LEAVES_DECAY_CHECK_BIT))
					continue;
				server_world_set_block_metadata_quiet(
					block_world(s), x + dx, y + dy, z + dz,
					(uint8_t)(b.metadata | BLOCK_LEAVES_DECAY_CHECK_BIT));
			}
		}
	}
}

/* BlockLeaves.onBlockRemoval [740c583 BlockLeaves.java:33-50]: var5 = 1. */
static void onBlockRemoval(struct server_local* s, struct block_info* this) {
	flagLeavesForDecay(s, this->x, this->y, this->z, 1, false);
}

/* BlockLog.onBlockRemoval [740c583 BlockLog.java:23-42]: var5 = 4, only
 * unflagged leaves are written. Wired from block_log.c's vtable. */
void block_leaves_on_log_removal(struct server_local* s,
								 struct block_info* info) {
	flagLeavesForDecay(s, info->x, info->y, info->z, 4, true);
}

/* BlockLeaves.removeLeaves [740c583 BlockLeaves.java:146-149]: drop FIRST
 * (dropBlockAsItem -> quantityDropped nextInt(20), then per unit nextFloat
 * <= 1.0F and the three dropBlockAsItem_do position floats -- all on
 * World.rand == s->rand_src), THEN setBlockWithNotify(0), whose
 * Chunk.setBlockID fires this block's own onBlockRemoval on the neighbours. */
static void removeLeaves(struct server_local* s, struct block_info* this) {
	server_local_spawn_block_drops(s, this);
	server_world_set_block(block_world(s), this->x, this->y, this->z,
						   (struct block_data) {
							   .type = BLOCK_AIR,
							   .metadata = 0,
						   });
}

/* BlockLeaves.updateTick [740c583 BlockLeaves.java:52-144], server side
 * (`!multiplayerWorld`: CaveWii's server is always the authority). */
static void onRandomTick(struct server_local* s, struct block_info* this) {
	const int var6 = this->block->metadata;
	if(!(var6 & BLOCK_LEAVES_DECAY_CHECK_BIT))
		return;
	const int var7 = LEAVES_WALK_RADIUS;
	const int var8 = var7 + 1;
	const w_coord_t x = this->x, y = this->y, z = this->z;
	int8_t* a = adjacentTreeBlocks;

	if(checkChunksExist(block_world(s), x - var8, y - var8, z - var8, x + var8,
						y + var8, z + var8)) {
		/* Region fill [:118-131]: 0 = wood, -2 = leaves, -1 = anything else
		 * (getBlockId returns 0/air outside the world -> -1). */
		for(int var12 = -var7; var12 <= var7; var12++) {
			for(int var13 = -var7; var13 <= var7; var13++) {
				for(int var14 = -var7; var14 <= var7; var14++) {
					struct block_data b;
					int8_t v = -1;
#ifdef CAVEWII_TESTING
					s_scan_lookups++;
#endif
					if(server_world_get_block(block_world(s), x + var12, y + var13,
											  z + var14, &b)) {
						if(b.type == BLOCK_LOG)
							v = 0;
						else if(b.type == BLOCK_LEAVES)
							v = -2;
					}
					a[LEAVES_SCRATCH_IDX(var12, var13, var14)] = v;
				}
			}
		}
		/* Distance passes [:76-115]: level d marks every unvisited leaf (-2)
		 * that is 6-adjacent to a cell at level d-1, for d = 1..4. Writes
		 * inside a pass are never re-read by the same pass (they hold d, the
		 * pass looks for d-1), so this is the level-synchronous BFS Beta runs. */
		for(int var12 = 1; var12 <= 4; var12++) {
			for(int var13 = -var7; var13 <= var7; var13++) {
				for(int var14 = -var7; var14 <= var7; var14++) {
					for(int var15 = -var7; var15 <= var7; var15++) {
						if(a[LEAVES_SCRATCH_IDX(var13, var14, var15)]
						   != var12 - 1)
							continue;
						int8_t* n;
						n = &a[LEAVES_SCRATCH_IDX(var13 - 1, var14, var15)];
						if(*n == -2)
							*n = (int8_t)var12;
						n = &a[LEAVES_SCRATCH_IDX(var13 + 1, var14, var15)];
						if(*n == -2)
							*n = (int8_t)var12;
						n = &a[LEAVES_SCRATCH_IDX(var13, var14 - 1, var15)];
						if(*n == -2)
							*n = (int8_t)var12;
						n = &a[LEAVES_SCRATCH_IDX(var13, var14 + 1, var15)];
						if(*n == -2)
							*n = (int8_t)var12;
						n = &a[LEAVES_SCRATCH_IDX(var13, var14, var15 - 1)];
						if(*n == -2)
							*n = (int8_t)var12;
						n = &a[LEAVES_SCRATCH_IDX(var13, var14, var15 + 1)];
						if(*n == -2)
							*n = (int8_t)var12;
					}
				}
			}
		}
	}
	/* [:135-140] -- read even when the chunk gate failed: Beta then judges
	 * by whatever the PREVIOUS updateTick left in the centre cell (0 on a
	 * fresh array -> the leaf survives and is unflagged). Replicated on
	 * purpose; the scratch is never cleared. */
	const int var12 = a[LEAVES_SCRATCH_IDX(0, 0, 0)];
	if(var12 >= 0)
		server_world_set_block_metadata_quiet(
			block_world(s), x, y, z, (uint8_t)(var6 & ~BLOCK_LEAVES_DECAY_CHECK_BIT & 0xF));
	else
		removeLeaves(s, this);
}

/* ItemLeaves.getPlacedBlockMetadata [740c583 ItemLeaves.java:10-12]:
 * `return var1 | 8;` -- a placed leaf carries its species bits AND the
 * check-decay flag, so it decays on its first random tick unless a log is
 * within 4 leaf steps. 1.7.3 has no "player-placed / never decays" bit. */
static bool onItemPlace(struct server_local* s, struct server_player* actor,
						struct item_data* it, struct block_info* where,
						struct block_info* on, enum side on_side) {
	struct item_data placed = *it;
	placed.durability
		= (uint16_t)(it->durability | BLOCK_LEAVES_DECAY_CHECK_BIT);
	return block_place_default(s, actor, &placed, where, on, on_side);
}

struct block block_leaves = {
	.name = "Leaves",
	.getSideMask = getSideMask,
	.getBoundingBox = getBoundingBox,
	.getMaterial = getMaterial,
	.getTextureIndex = getTextureIndex,
	.getDroppedItem = getDroppedItem,
	.onBlockRemoval = onBlockRemoval,
	.onRandomTick = onRandomTick,
	.onRightClick = NULL,
	.transparent = false,
	.renderBlock = render_block_full,
	.renderBlockAlways = NULL,
	.biome_tint = BIOME_TINT_FOLIAGE, // all faces tinted by biome foliage colour
	.luminance = 0,
	.double_sided = false,
	.can_see_through = true,
	.opacity = 1,
	.ignore_lighting = false,
	.flammable = true,
	.place_ignore = false,
	.digging.hardness = 300,
	.digging.tool = TOOL_TYPE_ANY,
	.digging.min = TOOL_TIER_ANY,
	.digging.best = TOOL_TIER_ANY,
	.block_item = {
		.has_damage = false,
		.max_stack = 64,
		.renderItem = render_item_block,
		.onItemPlace = onItemPlace, // ItemLeaves: damage | 8 (check-decay)
		.fuel = 0,
		.render_data.block.has_default = false,
		.armor.is_armor = false,
		.tool.type = TOOL_TYPE_ANY,
	},
};
