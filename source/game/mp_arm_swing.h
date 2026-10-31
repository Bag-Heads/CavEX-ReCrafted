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
	Multiplayer arm swing (card m1, docs/intent/m1-mp-arm-swing.md): WHEN the
	local player's client puts Packet18Animation(entity, 1) on the wire.

	Beta 1.7.3 sends it from exactly one place, EntityClientPlayerMP.swingItem
	(EntityClientPlayerMP.java:118-121: super.swingItem(); addToSendQueue(new
	Packet18Animation(this, 1))). EntityPlayerSP (single player) sends nothing.
	Every LOCAL caller of swingItem in the client:

	  1. Minecraft.clickMouse(0) -- the LEFT click -- swings FIRST
	     (Minecraft.java:789-791), before attackEntity's Packet7
	     (PlayerControllerMP.java:159-163) or clickBlock's Packet14 status 0
	     (PlayerControllerMP.java:41-43), and on a click into air too.
	     clickMouse(0) runs on the press (Minecraft.java:1098-1100) and again
	     every ticksPerSecond/4 = 5 ticks while the button is held
	     (Minecraft.java:1000-1003; mouseTicksRan is re-stamped even when the
	     call is gated). A click into air sets leftClickCounter = 10
	     (Minecraft.java:795-797), which blocks the whole clickMouse(0) until
	     it has been decremented back to 0, once per tick (Minecraft.java:
	     991-993) -- an air swing therefore repeats every 10 ticks, a block or
	     entity swing every 5.
	  2. Minecraft.clickMouse(1) -- the RIGHT click at a block -- swings AFTER
	     PlayerControllerMP.sendPlaceBlock's Packet15 (PlayerControllerMP.java:
	     141-146), and only when it returned true (Minecraft.java:817-820):
	     the target's blockActivated, else the held item's useItem ->
	     onItemUse (PlayerController.java:75-78). No swing on a right click at
	     an entity (interactWithEntity) or on a plain sendUseItem.
	  3. ItemFishingRod.onItemRightClick swings in BOTH branches, cast and
	     reel (ItemFishingRod.java:22,29) -- after sendUseItem's Packet15(-1)
	     (PlayerControllerMP.java:148-153). ItemBow/ItemEgg/ItemSnowball/
	     ItemBoat never swing.
	  (NetClientHandler.java:356 swings a REMOTE entity on an inbound Packet18;
	  that is the receive half, already wired as CRPC_ENTITY_ANIMATION.)

	Everything here is pure (no gstate, no world), so the cadence and the
	place predicate are golden-tested against the decompile directly
	(tests/test_mp_arm_swing.c). screen_ingame.c owns the call sites.
*/

#ifndef MP_ARM_SWING_H
#define MP_ARM_SWING_H

#include <stdbool.h>
#include <stdint.h>

#include "../block/blocks.h"
#include "../item/item_types.h"

/* Minecraft.java:1000: (ticksRan - mouseTicksRan) >= timer.ticksPerSecond /
 * 4.0F, ticksPerSecond = 20 -> 5 ticks x 50 ms. */
#define MP_SWING_REPEAT_MS 250
/* Minecraft.java:796: leftClickCounter = 10 -> 10 ticks x 50 ms. */
#define MP_SWING_AIR_LOCKOUT_MS 500

/* The held-left-button clock of Minecraft.runTick: mouseTicksRan and
 * leftClickCounter, kept in milliseconds (this engine's input runs per frame
 * on the wall clock; 1 tick = 50 ms). Zero-initialized = "never clicked". */
struct mp_swing_clock {
	bool clicked;			  /* mouseTicksRan has ever been stamped */
	int64_t last_click_ms;	  /* mouseTicksRan */
	int64_t lockout_until_ms; /* leftClickCounter > 0 until this time */
};

/* One frame of Minecraft's left-click driver. `pressed` = the button's press
 * edge this frame (the Mouse event path), `held` = the button is down (the
 * per-tick path), `air` = nothing under the crosshair (objectMouseOver ==
 * null). Returns true when clickMouse(0) runs AND passes its
 * leftClickCounter gate -- i.e. when Beta calls swingItem. */
static inline bool mp_swing_left_click(struct mp_swing_clock* c, int64_t now_ms,
									   bool pressed, bool held, bool air) {
	bool click = pressed
		|| (held
			&& (!c->clicked || now_ms - c->last_click_ms >= MP_SWING_REPEAT_MS));
	if(!click)
		return false;
	/* mouseTicksRan = ticksRan follows EVERY clickMouse call, gated or not. */
	c->clicked = true;
	c->last_click_ms = now_ms;
	/* clickMouse: if(var1 != 0 || this.leftClickCounter <= 0) {...} */
	if(now_ms < c->lockout_until_ms)
		return false;
	if(air)
		c->lockout_until_ms = now_ms + MP_SWING_AIR_LOCKOUT_MS;
	return true;
}

/* A left click this engine turns into an action on a frame the Beta driver
 * above did not click (this engine starts a dig on a held button once its own
 * 250 ms post-break cooldown ends, and attacks on every press even inside the
 * air lockout). Beta only ever sends that Packet14(0)/Packet7 from inside
 * clickMouse(0), right after the swing -- so the caller swings first and the
 * clock restarts from this click, exactly as a Beta click would. */
static inline void mp_swing_left_click_forced(struct mp_swing_clock* c,
											  int64_t now_ms) {
	c->clicked = true;
	c->last_click_ms = now_ms;
}

/* ItemFishingRod.java:22,29: the only item whose onItemRightClick swings. */
static inline bool mp_swing_after_use_item(bool has_item, uint16_t item_id) {
	return has_item && item_id == ITEM_FISHING_ROD;
}

/* Beta World.getBlockId / getBlockMetadata for the place predicate. `block`
 * must return 0 outside 0..127, like World.getBlockId. */
struct mp_swing_world {
	void* ctx;
	uint8_t (*block)(void* ctx, int x, int y, int z);
	uint8_t (*meta)(void* ctx, int x, int y, int z);
};

/* enum side -> Beta face (0 -Y, 1 +Y, 2 -Z, 3 +Z, 4 -X, 5 +X); the same
 * mapping server_remote_protocol.c's remote_side_to_face puts on the wire. */
static inline int mp_swing_beta_face(enum side side) {
	switch(side) {
		case SIDE_BOTTOM: return 0;
		case SIDE_TOP: return 1;
		case SIDE_FRONT: return 2;
		case SIDE_BACK: return 3;
		case SIDE_LEFT: return 4;
		case SIDE_RIGHT: return 5;
		default: return 1;
	}
}

/* ItemBlock/ItemReed/ItemFlintAndSteel/ItemRedstone/ItemSign's shared face
 * step: 0 --y, 1 ++y, 2 --z, 3 ++z, 4 --x, 5 ++x. */
static inline void mp_swing_face_step(int face, int* x, int* y, int* z) {
	if(face == 0)
		--*y;
	if(face == 1)
		++*y;
	if(face == 2)
		--*z;
	if(face == 3)
		++*z;
	if(face == 4)
		--*x;
	if(face == 5)
		++*x;
}

/* Material.getIsGroundCover by block id: MaterialTransparent (air, fire),
 * MaterialLiquid (water, lava) and Material.snow (the snow LAYER, id 78) set
 * it (Material.java:4,10,11,16,23; MaterialTransparent.java:6;
 * MaterialLiquid.java:6). */
static inline bool mp_swing_ground_cover(uint8_t id) {
	return id == 0 || id == BLOCK_WATER_FLOW || id == BLOCK_WATER_STILL
		|| id == BLOCK_LAVA_FLOW || id == BLOCK_LAVA_STILL || id == BLOCK_FIRE
		|| id == BLOCK_SNOW;
}

/* Block.canPlaceBlockAt (Block.java:471-474). */
static inline bool mp_swing_base_can_place_at(const struct mp_swing_world* w,
											  int x, int y, int z) {
	return mp_swing_ground_cover(w->block(w->ctx, x, y, z));
}

/* World.canBlockBePlacedAt(id, x, y, z, false, face) as far as the client
 * can read it: the OLD block must be air or one of the five ids the method
 * nulls (water/lava moving+still, fire, snow layer). DOCUMENTED RESIDUAL
 * (docs/intent/m1-mp-arm-swing.md): the two remaining conjuncts -- the new
 * block's canPlaceBlockOnSide support rule and checkIfAABBIsClear against
 * entities -- are predicted TRUE. Their per-block rules live only in the
 * server's placement callbacks; a second client copy would be a second
 * simulation to keep in sync, for a cosmetic swing. */
static inline bool mp_swing_can_block_be_placed_at(
	const struct mp_swing_world* w, int x, int y, int z) {
	uint8_t old = w->block(w->ctx, x, y, z);
	return old == 0 || old == BLOCK_WATER_FLOW || old == BLOCK_WATER_STILL
		|| old == BLOCK_LAVA_FLOW || old == BLOCK_LAVA_STILL
		|| old == BLOCK_FIRE || old == BLOCK_SNOW;
}

/* The target's blockActivated on a MULTIPLAYER client world returns true. */
static inline bool mp_swing_block_activated(uint8_t id, uint8_t meta) {
	switch(id) {
		case BLOCK_DISPENSER:		   /* BlockDispenser.java:68-70 */
		case BLOCK_NOTEBLOCK:		   /* BlockNote: multiplayerWorld -> true */
		case BLOCK_BED:				   /* BlockBed: multiplayerWorld -> true */
		case BLOCK_CHEST:			   /* BlockChest: every branch true */
		case BLOCK_WORKBENCH:		   /* BlockWorkbench: multiplayerWorld */
		case BLOCK_FURNACE:			   /* BlockFurnace.java:92-94 */
		case BLOCK_FURNACE_LIT:		   /* (same class, active id) */
		case BLOCK_DOOR_WOOD:		   /* BlockDoor: true (toggles) */
		case BLOCK_DOOR_IRON:		   /* BlockDoor: Material.iron -> true */
		case BLOCK_LEVER:			   /* BlockLever: multiplayerWorld */
		case BLOCK_STONE_BUTTON:	   /* BlockButton: true on both paths */
		case BLOCK_CAKE:			   /* BlockCake: true */
		case BLOCK_REDSTONE_REPEATER:  /* BlockRedstoneRepeater: true */
		case BLOCK_REDSTONE_REPEATER_LIT:
		case BLOCK_TRAP_DOOR:		   /* BlockTrapDoor: true */
			return true;
		case BLOCK_JUKEBOX: /* BlockJukeBox: metadata 0 (empty) -> false */
			return meta != 0;
		default:
			/* Block.blockActivated false; BlockStairs defers to planks/
			 * cobblestone (false); BlockPistonMoving on a multiplayer world,
			 * BlockPistonBase, BlockRedstoneOre and BlockTNT all false. */
			return false;
	}
}

static inline bool mp_swing_is_rail(uint8_t id) {
	/* BlockRail.isRailBlock: rail, powered rail, detector rail. */
	return id == BLOCK_RAIL || id == BLOCK_POWERED_RAIL
		|| id == BLOCK_DETECTOR_RAIL;
}

/* ItemBlock.onItemUse (ItemBlock.java) / ItemReed.onItemUse (ItemReed.java).
 * `check_top` is ItemBlock's `y == 127 && material.isSolid()` refusal, which
 * ItemReed does not have. ItemReed returns true for every nonempty stack,
 * even when the stepped destination cannot accept the block. */
static inline bool mp_swing_block_item_use(const struct mp_swing_world* w,
										   uint8_t placed, uint8_t count,
										   bool check_top, int x, int y, int z,
										   int face) {
	if(w->block(w->ctx, x, y, z) != BLOCK_SNOW)
		mp_swing_face_step(face, &x, &y, &z);
	if(count == 0)
		return false;
	if(!check_top) /* ItemReed.onItemUse: placement failure still returns true. */
		return true;
	if(check_top && y == 127 && block_beta_material_is_solid(placed))
		return false;
	return placed > 0 && mp_swing_can_block_be_placed_at(w, x, y, z);
}

/* PlayerController.sendPlaceBlock (PlayerController.java:75-78):
 *   id > 0 && blockActivated ? true : (stack == null ? false : useItem(...))
 * -> the Item.onItemUse of every Beta item that overrides it; everything else
 * inherits Item.onItemUse == false. `yaw_deg` is Beta rotationYaw (ItemBed
 * derives its foot direction from it). */
static inline bool mp_swing_after_place(const struct mp_swing_world* w, int x,
										int y, int z, int face, bool has_item,
										uint16_t item_id, uint16_t damage,
										uint8_t count, float yaw_deg) {
	uint8_t target = w->block(w->ctx, x, y, z);
	if(target > 0 && mp_swing_block_activated(target, w->meta(w->ctx, x, y, z)))
		return true;
	if(!has_item)
		return false;

	if(item_id > 0 && item_id < 256)
		return mp_swing_block_item_use(w, (uint8_t)item_id, count, true, x, y,
									   z, face);

	switch(item_id) {
		/* ItemReed(338 -> Block.reed 83, 354 -> Block.cake 92, 356 ->
		 * Block.redstoneRepeaterIdle 93) (Item.java). */
		case ITEM_REED:
			return mp_swing_block_item_use(w, BLOCK_REED, count, false, x, y, z,
										   face);
		case ITEM_CAKE:
			return mp_swing_block_item_use(w, BLOCK_CAKE, count, false, x, y, z,
										   face);
		case ITEM_REDSTONE_REPEATER:
			return mp_swing_block_item_use(w, BLOCK_REDSTONE_REPEATER, count,
										   false, x, y, z, face);
		case ITEM_FLINT_STEEL: /* ItemFlintAndSteel: always true */
			return true;
		case ITEM_WOOD_HOE:
		case ITEM_STONE_HOE:
		case ITEM_IRON_HOE:
		case ITEM_DIAMOND_HOE:
		case ITEM_GOLD_HOE: {
			/* ItemHoe: if((face == 0 || above != 0 || id != grass)
			 *            && id != dirt) return false; */
			uint8_t above = w->block(w->ctx, x, y + 1, z);
			return !((face == 0 || above != 0 || target != BLOCK_GRASS)
					 && target != BLOCK_DIRT);
		}
		case ITEM_SEED: /* ItemSeeds: top face of farmland, air above */
			return face == 1 && target == BLOCK_FARMLAND
				&& w->block(w->ctx, x, y + 1, z) == 0;
		case ITEM_DYE: /* ItemDye: bone meal on sapling / crops / grass */
			return damage == 15
				&& (target == BLOCK_SAPLING || target == BLOCK_CROPS
					|| target == BLOCK_GRASS);
		case ITEM_BED: {
			/* ItemBed: top face; foot at y+1 plus the yaw-derived head cell,
			 * both air over normal cubes. */
			if(face != 1)
				return false;
			int by = y + 1;
			/* MathHelper.floor_double((double)(yaw * 4.0F / 360.0F) + 0.5D) & 3 */
			double q = (double)(yaw_deg * 4.0F / 360.0F) + 0.5;
			int64_t fq = (int64_t)q;
			if(q < (double)fq)
				fq--;
			int dir = (int)(fq & 3);
			int dx = 0, dz = 0;
			if(dir == 0)
				dz = 1;
			if(dir == 1)
				dx = -1;
			if(dir == 2)
				dz = -1;
			if(dir == 3)
				dx = 1;
			return w->block(w->ctx, x, by, z) == 0
				&& w->block(w->ctx, x + dx, by, z + dz) == 0
				&& block_is_normal_cube(w->block(w->ctx, x, by - 1, z))
				&& block_is_normal_cube(w->block(w->ctx, x + dx, by - 1, z + dz));
		}
		case ITEM_DOOR_WOOD:
		case ITEM_DOOR_IRON: {
			/* ItemDoor: top face; BlockDoor.canPlaceBlockAt at y+1
			 * (BlockDoor.java:184-186). The hinge/facing math after it never
			 * changes the return value. */
			if(face != 1)
				return false;
			int dy = y + 1;
			return dy < 127
				&& block_is_normal_cube(w->block(w->ctx, x, dy - 1, z))
				&& mp_swing_base_can_place_at(w, x, dy, z)
				&& mp_swing_base_can_place_at(w, x, dy + 1, z);
		}
		case ITEM_MINECART:
		case ITEM_MINECART_CHEST:
		case ITEM_MINECART_FURNACE: /* ItemMinecart: on any rail */
			return mp_swing_is_rail(target);
		case ITEM_PAINTING: /* ItemPainting: any side face, fit or not */
			return face >= 2;
		case ITEM_RECORD_13:
		case ITEM_RECORD_CAT: /* ItemRecord: an empty jukebox */
			return target == BLOCK_JUKEBOX && w->meta(w->ctx, x, y, z) == 0;
		case ITEM_REDSTONE: {
			/* ItemRedstone: the stepped cell must be air (snow is used in
			 * place); after that it returns true whether or not the wire
			 * fits. */
			if(target != BLOCK_SNOW) {
				int rx = x, ry = y, rz = z;
				mp_swing_face_step(face, &rx, &ry, &rz);
				if(w->block(w->ctx, rx, ry, rz) != 0)
					return false;
			}
			return true;
		}
		case ITEM_SIGN: {
			/* ItemSign: not the bottom face; the clicked block's material
			 * solid; then Block.signPost.canPlaceBlockAt at the stepped cell
			 * (BlockSign inherits Block.canPlaceBlockAt). */
			if(face == 0 || !block_beta_material_is_solid(target))
				return false;
			int sx = x, sy = y, sz = z;
			mp_swing_face_step(face, &sx, &sy, &sz);
			return mp_swing_base_can_place_at(w, sx, sy, sz);
		}
		default:
			return false; /* Item.onItemUse */
	}
}

#endif
