#ifndef CAVEWII_ITEM_BUCKET_H
#define CAVEWII_ITEM_BUCKET_H

#include <stdbool.h>
#include <stdint.h>

#include "../item_types.h"

struct server_local;
struct server_player;
struct item_data;

/* The four Beta ItemBucket instances (Item.java:77-79,87). */
static inline bool item_bucket_is_bucket(uint16_t id) {
	return id == ITEM_BUCKET || id == ITEM_BUCKET_WATER
		|| id == ITEM_BUCKET_LAVA || id == ITEM_MILK_BUCKET;
}

/* Beta ItemBucket.onItemRightClick for EVERY local bucket right-click, with or
 * without a block under the crosshair (ItemBucket has no onItemUse, so the
 * at-a-block SRPC_BLOCK_PLACE dispatch and the no-target SRPC_USE_ITEM path
 * both land here; the clicked block and face are never used). Does Beta's own 5.0-block ray from
 * the actor's eye (liquids hittable only for the EMPTY bucket, sources only)
 * and applies the result to the world. *held is replaced by the stack Beta
 * returns. Returns true when Beta's returned stack differs from the input
 * (PlayerController.sendUseItem then writes the slot); false is a no-op.
 * The caller owns the inventory slot, creative restore and client resync.
 * [ItemBucket.java:12-99] */
bool item_bucket_use_no_target(struct server_local* s,
							   struct server_player* actor,
							   struct item_data* held);

#endif
