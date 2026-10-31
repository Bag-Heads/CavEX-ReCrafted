/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

#include "../../network/server_local.h"
#include "../../network/server_local_state.h" // full server state (split from server_local.h)
#include "../../network/client_interface.h"
#include "../../network/server_interface.h"
#include "../../block/blocks.h"

#include "../../item/items.h"           // ITEM_BUCKET, ITEM_BUCKET_WATER, ITEM_BUCKET_LAVA
#include "../../item/items_object.h"    // item_bucket_water / item_bucket_lava (filled-bucket place)
#include "../../item/inventory.h"
#include "../../item/window_container.h"// WINDOWC_INVENTORY
#include "../../network/server_local.h" // server_local_send_inv_changes
#include "../../network/inventory_logic.h" // set_inv_slot_*
#include "../../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../../block/aabb.h"
#include "../../block/redstone_piston_server.h" // piston_moving_state_lookup
#include "../../entity/block_ray.h"
#include "../../worldgen/beta_math.h"           // mh_sin/mh_cos (MathHelper table)
#include "item_bucket.h"

// Beta ItemBucket fill gate: a fillable SOURCE is water/lava material with metadata 0 (a full
// block). Returns the filled-bucket item id, or 0 if `bd` is not a source. Beta keys on
// material+metadata, so accept the FLOW id at metadata 0 too -- a freshly placed bucket source
// is FLOW(meta 0) until it settles to STILL, and Beta would scoop it. A non-source STILL block
// (settled partial flow, metadata>0) is NOT scoopable. [decompile 740c583 ItemBucket]
static uint16_t bucket_source_fill_id(struct block_data bd)
{
    if (bd.metadata != 0) return 0;
    if (bd.type == BLOCK_WATER_STILL || bd.type == BLOCK_WATER_FLOW) return ITEM_BUCKET_WATER;
    if (bd.type == BLOCK_LAVA_STILL  || bd.type == BLOCK_LAVA_FLOW)  return ITEM_BUCKET_LAVA;
    return 0;
}

/* ---- BUCKET-WATER-OFFLINE: the no-target use (Beta's own liquid ray) ----
 *
 * Beta runs ItemBucket.onItemRightClick from Minecraft.clickMouse's trailing
 * sendUseItem for any held stack; the ordinary crosshair ray passes through
 * liquids, so aiming at open water reaches here with no block target.
 * [mcb173-retromcp ItemBucket.java:12-99] */

struct bucket_ray_ctx {
	struct server_local* s;
	struct server_world* w;
};

static bool bucket_is_fluid(uint8_t t) {
	return t == BLOCK_WATER_FLOW || t == BLOCK_WATER_STILL
		|| t == BLOCK_LAVA_FLOW || t == BLOCK_LAVA_STILL;
}

static void bucket_ray_unit(struct block_ray_cell* out) {
	out->has_ray_box = true;
	out->x1 = 0.0; out->y1 = 0.0; out->z1 = 0.0;
	out->x2 = 1.0; out->y2 = 1.0; out->z2 = 1.0;
}

/* World.func_28105_a's per-cell facts with var4 == false (rayTraceBlocks_do):
 * collision presence is NOT consulted, only canCollideCheck + the block's
 * collisionRayTrace bounds. [World.java:700-718]
 *   BlockFluid.canCollideCheck: `var2 && var1 == 0` (sources only, and only
 *     when the caller asks for liquids) [BlockFluid.java:56-58]; its bounds are
 *     the full cube (CaveWii's fluid selection box is 0, so it is explicit).
 *   BlockFire.isCollidable false [BlockFire.java:156-158].
 *   Everything else: Block.canCollideCheck -> isCollidable() true
 *     [Block.java:275-281]; bounds = setBlockBoundsBasedOnState, i.e. the
 *     selection box (cactus keeps the unit block, see entity_arrow.c). */
static bool bucket_ray_cell(int x, int y, int z, bool stop_on_liquid,
							struct block_ray_cell* out, void* vctx) {
	struct bucket_ray_ctx* ctx = vctx;
	struct block_data cell;
	*out = (struct block_ray_cell) {0};
	if(!server_world_get_block(ctx->w, x, y, z, &cell))
		return false; /* unloaded: never an invented solid */
	if(cell.type == BLOCK_AIR || !blocks[cell.type])
		return true;
	out->present = true;
	if(bucket_is_fluid(cell.type)) {
		out->collidable = stop_on_liquid && cell.metadata == 0;
		if(out->collidable)
			bucket_ray_unit(out);
		return true;
	}
	out->collidable = cell.type != BLOCK_FIRE;
	if(!out->collidable)
		return true;
	struct block_info info = {.block = &cell, .x = x, .y = y, .z = z,
		.get_piston_moving = piston_moving_state_lookup,
		.get_piston_moving_ctx = ctx->s};
	struct AABB ray;
	if(cell.type == BLOCK_CACTUS) {
		bucket_ray_unit(out);
	} else if(blocks[cell.type]->getBoundingBox
			  && blocks[cell.type]->getBoundingBox(&info, false, NULL) == 1) {
		blocks[cell.type]->getBoundingBox(&info, false, &ray);
		out->has_ray_box = true;
		out->x1 = ray.x1; out->y1 = ray.y1; out->z1 = ray.z1;
		out->x2 = ray.x2; out->y2 = ray.y2; out->z2 = ray.z2;
	} else if(cell.type != BLOCK_PISTON_MOVING) {
		/* Every Beta block has bounds (default unit cube); a CaveWii block
		 * that registers no selection box is still hit as the unit cube. An
		 * orphan moving piston (no TE) has no box, as in entity.c. */
		bucket_ray_unit(out);
	}
	return true;
}

/* enum side for a Beta sideHit (0 -Y, 1 +Y, 2 -Z, 3 +Z, 4 -X, 5 +X), the
 * inverse of mp_swing_beta_face / remote_side_to_face. */
static enum side bucket_side_from_beta(int face) {
	switch(face) {
		case 0: return SIDE_BOTTOM;
		case 1: return SIDE_TOP;
		case 2: return SIDE_FRONT;
		case 3: return SIDE_BACK;
		case 4: return SIDE_LEFT;
		default: return SIDE_RIGHT;
	}
}

bool item_bucket_use_no_target(struct server_local* s,
							   struct server_player* actor,
							   struct item_data* held) {
	if(!s || !actor || !held || held->count == 0)
		return false;
	struct item* filled_item = NULL;
	bool empty = held->id == ITEM_BUCKET;
	bool milk = held->id == ITEM_MILK_BUCKET;
	if(held->id == ITEM_BUCKET_WATER)
		filled_item = &item_bucket_water;
	else if(held->id == ITEM_BUCKET_LAVA)
		filled_item = &item_bucket_lava;
	else if(!empty && !milk)
		return false;
	struct server_world* w = block_world(s);
	if(!w || !actor->has_pos)
		return false;

	/* ItemBucket.java:13-27 with partialTick var4 = 1.0F (current pose).
	 * server_player.y is the EYE (posY + 1.62 - yOffset), rx = rotationYaw,
	 * ry = rotationPitch, all in Beta degrees. Float trig through the
	 * MathHelper table, double endpoint at 5.0D. */
	const float deg = (float)3.141592653589793 / 180.0F;
	const float pi_f = (float)3.141592653589793;
	float pitch = actor->ry;
	float yaw = actor->rx;
	float c_yaw = mh_cos(-yaw * deg - pi_f);
	float s_yaw = mh_sin(-yaw * deg - pi_f);
	float c_pitch = -mh_cos(-pitch * deg);
	float s_pitch = mh_sin(-pitch * deg);
	float dx = s_yaw * c_pitch;
	float dz = c_yaw * c_pitch;
	const double reach = 5.0;
	double from[3] = {actor->x, actor->y, actor->z};
	double to[3] = {from[0] + (double)dx * reach,
		from[1] + (double)s_pitch * reach, from[2] + (double)dz * reach};

	/* rayTraceBlocks_do(var13, var23, isFull == 0) [ItemBucket.java:28]. */
	struct bucket_ray_ctx ctx = {.s = s, .w = w};
	struct block_ray_hit hit;
	if(!block_ray_trace(from, to, empty, false, bucket_ray_cell, &ctx, &hit,
						NULL))
		return false; /* var24 == null -> return var1 */
	/* World.func_6466_a is `return true` in single player [World.java:2281]. */

	if(empty) {
		/* ItemBucket.java:41-49: water / lava MATERIAL at metadata 0. */
		struct block_data bd;
		if(!server_world_get_block(w, hit.x, hit.y, hit.z, &bd))
			return false;
		uint16_t filled_id = bucket_source_fill_id(bd);
		if(!filled_id)
			return false;
		/* setBlockWithNotify(x, y, z, 0) [ItemBucket.java:42,47]. */
		if(!server_world_set_block(w, hit.x, hit.y, hit.z, (struct block_data) {
			.type = BLOCK_AIR, .metadata = 0, .sky_light = 0, .torch_light = 0,
		}))
			return false;
		*held = (struct item_data) {.id = filled_id, .count = 1, .durability = 0};
		return true;
	}
	if(milk) {
		/* isFull < 0 on any tile hit: `return new ItemStack(bucketEmpty)`
		 * (the milk is poured away, nothing placed) [ItemBucket.java:51-53]. */
		*held = (struct item_data) {.id = ITEM_BUCKET, .count = 1, .durability = 0};
		return true;
	}
	/* Full water/lava: face step + `isAirBlock || !isSolid()` + the nether
	 * water fizz + setBlockAndMetadataWithNotify(isFull, 0) is exactly the
	 * filled bucket's own onUseOnBlock on (hit block, sideHit), which swaps the
	 * stack to the empty bucket on success [ItemBucket.java:55-90]. */
	struct block_data hit_bd;
	if(!server_world_get_block(w, hit.x, hit.y, hit.z, &hit_bd))
		return false;
	struct block_info on = {.block = &hit_bd, .x = hit.x, .y = hit.y,
		.z = hit.z};
	struct item_data out = *held;
	if(!filled_item->onUseOnBlock(s, actor, &out, &on,
								  bucket_side_from_beta(hit.side)))
		return false;
	*held = out;
	return true;
}


struct item item_bucket = {
	.name = "Bucket",
	.has_damage = false,  // Disable damage (it won't be used up anymore)
	.max_damage = 0,      // Set max_damage to 0, so it doesn't get used up
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	/* No onUseOnBlock: every bucket click runs item_bucket_use_no_target
	 * (Beta ItemBucket has no onItemUse). */
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 10,
			.texture_y = 4,
		},
	},
};
