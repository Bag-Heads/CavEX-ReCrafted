/*
	Copyright (c) 2026 CaveWii contributors

	Beta 1.7.3 Block.slipperiness: every block defaults to 0.6F (Block.java:143);
	only ice overrides it, to 0.98F (BlockIce.java:8). Ground friction consumers
	(EntityLiving.moveEntityWithHeading, EntityItem.onUpdate) read the block under
	the entity and scale it by 0.91F themselves -- this helper returns the raw value.
	Shared by the next-gaps-0924 wave-1 player/mob/item lanes (P2).
*/

#ifndef BLOCK_SLIPPERINESS_H
#define BLOCK_SLIPPERINESS_H

#include "blocks_data.h"

#define BLOCK_SLIPPERINESS_DEFAULT 0.6F
#define BLOCK_SLIPPERINESS_ICE 0.98F

static inline float block_slipperiness(int block_id) {
	return block_id == BLOCK_ICE ? BLOCK_SLIPPERINESS_ICE
								 : BLOCK_SLIPPERINESS_DEFAULT;
}

#endif
