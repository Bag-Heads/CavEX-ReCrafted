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

#ifndef BLOCK_LEAVES_H
#define BLOCK_LEAVES_H

#include <stdbool.h>
#include <stdint.h>

#include "blocks_data.h"

struct server_local;
struct block_info;

/* Beta 1.7.3 leaf metadata layout [740c583 BlockLeaves.java]:
 *   bits 0-1  wood species (0 oak, 1 pine, 2 birch)   -- damageDropped:169-171
 *   bit  3    (8) "check decay" flag                   -- updateTick:55
 * There is NO player-placed / no-decay bit in 1.7.3 (that is 1.8's bit 4):
 * ItemLeaves.getPlacedBlockMetadata [ItemLeaves.java:10-12] returns
 * `damage | 8`, so a placed leaf is flagged on placement and decays on its
 * first random tick unless a log is within 4 leaf steps. */
#define BLOCK_LEAVES_DECAY_CHECK_BIT 8

/* Beta BlockLog.onBlockRemoval [740c583 BlockLog.java:23-42]: when a log is
 * replaced, every leaf in the 9x9x9 cube around it (radius 4) that is not yet
 * flagged gets the check-decay bit, provided every chunk within radius 5 is
 * loaded (World.checkChunksExist). The implementation lives in block_leaves.c
 * because it is the leaf-metadata contract; block_log.c only wires it into
 * its vtable. */
void block_leaves_on_log_removal(struct server_local* s,
								 struct block_info* info);

/* Beta graphicsLevel for leaves, from render.fancy_graphics (default off).
 *   fast (false, the default): BlockLeaves.setGraphicsLevel(false) picks the
 *     OPAQUE tile, blockIndexInTexture + 1 (oak/birch 53, spruce 133)
 *     [BlockLeaves.java:177-184], and isOpaqueCube() = !graphicsLevel = true
 *     [BlockLeaves.java:173-175]: a leaf hides and is hidden like stone, so
 *     leaf-to-leaf faces are culled [BlockLeavesBase.java:15-18].
 *   fancy (true): the cut-out tile (52 / 132) and every leaf-to-leaf face
 *     drawn, so the holes show the leaves behind.
 * Set once at boot, before any chunk is meshed; read by the mesher worker. */
void block_leaves_set_fancy(bool fancy);
bool block_leaves_fancy(void);

/* ColorizerFoliage [ColorizerFoliage.java:17-27]. */
#define BLOCK_LEAVES_PINE_RGB 0x619961u	 /* getFoliageColorPine */
#define BLOCK_LEAVES_BIRCH_RGB 0x80A755u /* getFoliageColorBirch */
#define BLOCK_LEAVES_OAK_ITEM_RGB 0x48B518u /* func_31073_c (item / no map) */

/* One leaf face's colour, shared by the chunk mesher and the piston-moving
 * renderer. Beta BlockLeaves.colorMultiplier [BlockLeaves.java:19-29]
 * checks meta & 1 FIRST (pine), then meta & 2 (birch), else the biome
 * foliage colour; the texture is the spruce tile only for (meta & 3) == 1,
 * else the oak tile [:177-179]. Returns the per-vertex tint (white = none)
 * and sets *raw_oak when the face must sample the RAW oak cell
 * (block_leaves_raw_oak_texture) instead of the block's own baked cell:
 *   meta 1 spruce: baked spruce cell (pine baked in), white;
 *   meta 2 birch:  baked birch cell (birch baked in), white;
 *   meta 3:        raw oak cell x pine;
 *   meta 0 oak:    raw oak cell x biome foliage when the column has a real
 *                  foliage colour, else the baked oak cell (0x48B518), white. */
static inline uint32_t block_leaves_face_tint(uint8_t meta, bool foliage_known,
											  uint32_t foliage_rgb,
											  bool* raw_oak) {
	uint8_t m = meta & 3;
	*raw_oak = false;
	if(m & 1) {
		if(m == 1)
			return 0xFFFFFFu;
		*raw_oak = true;
		return BLOCK_LEAVES_PINE_RGB;
	}
	if(m & 2)
		return 0xFFFFFFu;
	if(!foliage_known)
		return 0xFFFFFFu;
	*raw_oak = true;
	return foliage_rgb;
}

/* block_info.texture_override for a *raw_oak face: the raw oak cell of the
 * current graphics level. */
uint8_t block_leaves_raw_oak_texture(struct block_info* this, enum side side);

#ifdef CAVEWII_TESTING
/* Test/perf instrumentation: total server_world_get_block calls made by the
 * leaf updateTick region fill since process start (the Wii cost driver --
 * the 32^3 scratch itself is a static buffer, never malloc'd). */
uint32_t block_leaves_test_scan_lookups(void);
#endif

#endif
