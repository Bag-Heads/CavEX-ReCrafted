/*
	Bed block metadata -- Beta 1.7.3's own layout, used everywhere in this
	project (PAR-BED-META). Real Beta saves and a real server's 0x33/0x35
	chunk/block-change packets carry this layout verbatim, so every reader and
	writer must agree with it.

	[mcb173-retromcp BlockBed.java:7,135-145; ItemBed.java:14-35]
	  meta & 0x3  direction = BlockBed.getDirectionFromMetadata, indexing
	              headBlockToFootBlockMap {{0,1},{-1,0},{0,-1},{1,0}}:
	              0 = +Z (south), 1 = -X (west), 2 = -Z (north), 3 = +X (east).
	              The pair: foot (no 0x8) + map[dir] = the 0x8 half.
	  meta & 0x4  BlockBed.isBedOccupied (set by blockActivated after a
	              successful sleepInBedAt, cleared by wakeUpPlayer).
	  meta & 0x8  the pillow half ItemBed places at foot + map[dir] with
	              `var9 + 8`; MCP names the test isBlockFootOfBed, but this is
	              the half blockActivated walks TO and sleeps in, i.e. the HEAD.
	              Only the half WITHOUT 0x8 ever drops the bed item
	              (idDropped / dropBlockAsItemWithChance).

	Pure, dependency-free helpers so the save migration (region_archive.c),
	the client (camera.c, render_block.c) and the server share one decode.
*/

#ifndef BED_META_H
#define BED_META_H

#include <stdbool.h>
#include <stdint.h>

#define BED_META_DIRECTION 0x3
#define BED_META_OCCUPIED 0x4
#define BED_META_HEAD 0x8

static inline int bed_meta_direction(uint8_t meta) {
	return meta & BED_META_DIRECTION;
}

static inline bool bed_meta_is_head(uint8_t meta) {
	return (meta & BED_META_HEAD) != 0;
}

static inline bool bed_meta_is_occupied(uint8_t meta) {
	return (meta & BED_META_OCCUPIED) != 0;
}

/* BlockBed.headBlockToFootBlockMap[dir][0] / [1]: the foot -> head step. */
static inline int bed_meta_dx(uint8_t meta) {
	switch(meta & BED_META_DIRECTION) {
		case 1: return -1;
		case 3: return 1;
		default: return 0;
	}
}

static inline int bed_meta_dz(uint8_t meta) {
	switch(meta & BED_META_DIRECTION) {
		case 0: return 1;
		case 2: return -1;
		default: return 0;
	}
}

/* Which faces carry the bed's END textures (headboard on the 0x8 half,
 * footboard on the other): the X faces for an X-axis bed (directions 1/3),
 * the Z faces otherwise. BlockBed.getBlockTextureFromSideAndMetadata picks
 * the end tile through ModelBed.bedDirection[dir][side] in {2,3}, which lands
 * on sides 4/5 (X) exactly for dir 1/3. */
static inline bool bed_meta_ends_on_x_axis(uint8_t meta) {
	return (meta & 0x1) != 0;
}

/* ItemBed.onItemUse: var9 = floor(rotationYaw*4/360 + 0.5) & 3 IS the stored
 * direction (foot = var9, head = var9 + 8). */
static inline uint8_t bed_meta_from_yaw_index(int yaw_index, bool head) {
	return (uint8_t)((yaw_index & BED_META_DIRECTION)
					 | (head ? BED_META_HEAD : 0));
}

/* Save version 5 migration ONLY: CaveWii saves before version 5 stored the
 * direction as index<<1 over {+Z,-Z,+X,-X} (metadata 0/2/4/6, head |8, no
 * occupied bit, bit 0x1 never written). Maps that index onto Beta's
 * direction (+Z 0, -Z 2, +X 3, -X 1), keeps the head bit, and leaves the
 * occupied bit clear (the legacy layout had none). NOT idempotent by itself
 * (legacy 4 -> 3 would decode again as -Z): callers guard with the per-chunk
 * marker and the world save version. */
static inline uint8_t bed_meta_from_legacy(uint8_t legacy) {
	static const uint8_t legacy_index_to_beta[4] = {0, 2, 3, 1};
	return (uint8_t)(legacy_index_to_beta[(legacy >> 1) & 3]
					 | (legacy & BED_META_HEAD));
}

/* render_block_bed's SIDE_TOP tex_rotate: rotates the bed-top tile so its
 * +u axis (foot tile -> head tile -> pillow in terrain.png) points along the
 * foot->head direction, matching RenderBlocks.renderBlockBed's per-var7 top
 * UV assignment. Proven in tests/test_bed_beta_meta.c. */
static inline int bed_meta_top_rotation(uint8_t meta) {
	return 3 - (meta & BED_META_DIRECTION);
}

#endif
