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

#ifndef ITEM_TYPES_H
#define ITEM_TYPES_H

// Item VALUE TYPES (step 7 interface split, 2026-08-27): the
// item_data/item structs and every enum they embed, moved verbatim
// from items.h. By-value consumers (blocks.h embeds struct item in
// struct block) include THIS leaf; the item API + registry stay in
// items.h so item API edits no longer rebuild every block consumer.

#include <stdbool.h>
#include <stdint.h>

#include "../cglm/cglm.h"

struct item_data {
	uint16_t id;
	// uint16: Beta saves Damage as an NBT SHORT and this repo's own tools
	// define max_damage up to 1562 (diamond) -- the old uint8 truncated any
	// damage > 255 mod 256 through the save path (silent tool corruption;
	// 2026-06-11 feasibility-research find, pre-ship blocker P-1). Also the
	// future enchantment/item-tag work rides this width. Wool colour /
	// metadata uses (< 16) are unaffected.
	uint16_t durability;
	uint8_t count;
	/* Import record handle. Occupies the former padding byte (Wii: still 6 B).
	 * Zero is the canonical vanilla record; 255 is pinned to its saved scope. */
	uint8_t nbt_record;
};

static inline bool item_data_pinned(const struct item_data* item) {
	return item && item->nbt_record == UINT8_MAX;
}

enum item_type {
	ITEM_IRON_SHOVEL = 256,
	ITEM_IRON_PICKAXE = 257,
	ITEM_IRON_AXE = 258,
	ITEM_FLINT_STEEL = 259,
	ITEM_APPLE = 260,
	ITEM_BOW = 261,
	ITEM_ARROW = 262,
	ITEM_COAL = 263,
	ITEM_DIAMOND = 264,
	ITEM_IRON = 265,
	ITEM_GOLD = 266,
	ITEM_IRON_SWORD = 267,
	ITEM_WOOD_SWORD = 268,
	ITEM_WOOD_SHOVEL = 269,
	ITEM_WOOD_PICKAXE = 270,
	ITEM_WOOD_AXE = 271,
	ITEM_STONE_SWORD = 272,
	ITEM_STONE_SHOVEL = 273,
	ITEM_STONE_PICKAXE = 274,
	ITEM_STONE_AXE = 275,
	ITEM_DIAMOND_SWORD = 276,
	ITEM_DIAMOND_SHOVEL = 277,
	ITEM_DIAMOND_PICKAXE = 278,
	ITEM_DIAMOND_AXE = 279,
	ITEM_STICK = 280,
	ITEM_BOWL = 281,
	ITEM_MUSHROOM_STEW = 282,
	ITEM_GOLD_SWORD = 283,
	ITEM_GOLD_SHOVEL = 284,
	ITEM_GOLD_PICKAXE = 285,
	ITEM_GOLD_AXE = 286,
	ITEM_STRING = 287,
	ITEM_FEATHER = 288,
	ITEM_GUNPOWDER = 289,
	ITEM_WOOD_HOE = 290,
	ITEM_STONE_HOE = 291,
	ITEM_IRON_HOE = 292,
	ITEM_DIAMOND_HOE = 293,
	ITEM_GOLD_HOE = 294,
	ITEM_SEED = 295,
	ITEM_WHEAT = 296,
	ITEM_BREAD = 297,
	ITEM_LEATHER_HELMET = 298,
	ITEM_LEATHER_CHESTPLATE = 299,
	ITEM_LEATHER_LEGGINGS = 300,
	ITEM_LEATHER_BOOTS = 301,
	ITEM_CHAIN_HELMET = 302,
	ITEM_CHAIN_CHESTPLATE = 303,
	ITEM_CHAIN_LEGGINGS = 304,
	ITEM_CHAIN_BOOTS = 305,
	ITEM_IRON_HELMET = 306,
	ITEM_IRON_CHESTPLATE = 307,
	ITEM_IRON_LEGGINGS = 308,
	ITEM_IRON_BOOTS = 309,
	ITEM_DIAMOND_HELMET = 310,
	ITEM_DIAMOND_CHESTPLATE = 311,
	ITEM_DIAMOND_LEGGINGS = 312,
	ITEM_DIAMOND_BOOTS = 313,
	ITEM_GOLD_HELMET = 314,
	ITEM_GOLD_CHESTPLATE = 315,
	ITEM_GOLD_LEGGINGS = 316,
	ITEM_GOLD_BOOTS = 317,
	ITEM_FLINT = 318,
	ITEM_PORKCHOP = 319,
	ITEM_PORKCHOP_COOKED = 320,
	// Beta Item.painting: ItemPainting id 65 -> 256+65 [740c583: Item.java:73
	// static initializer]. maxStackSize stays the Item default (64): neither
	// ItemPainting.java constructor overrides it. Fully wired (painting-slice,
	// gap audit #9): onUseOnBlock drives the real EntityPainting placement
	// path (source/item/items/item_painting.c) -- see entity_painting.c/.h.
	ITEM_PAINTING = 321,
	ITEM_APPLE_GOLDEN = 322,
	// Beta Item.sign: ItemSign id 67, icon (10, 2) -> 256+67.
	// ItemSign's ctor sets maxStackSize = 1 explicitly (signs never stack).
	// ItemSign.onItemUse is wired (items_object.h's onItemPlace): top faces
	// place Block.signPost (BLOCK_SIGN_POST/63), side faces place Block.signWall
	// (BLOCK_SIGN/68) with Beta's raw wall metadata 2..5, and bottom
	// faces are refused. The post-place edit GUI opens from server_local.c's
	// SRPC_BLOCK_PLACE dispatch.
	// [740c583: Item.java static initializer, ItemSign.java]
	ITEM_SIGN = 323,
	ITEM_DOOR_WOOD = 324,
	ITEM_BUCKET = 325,
	ITEM_BUCKET_WATER = 326,
	ITEM_BUCKET_LAVA = 327,
	ITEM_MINECART = 328,
	ITEM_SADDLE = 329,
	ITEM_DOOR_IRON = 330,
	ITEM_REDSTONE = 331,
	ITEM_SNOW_BALL = 332,
	// Beta Item.boat: ItemBoat id 77, icon (8, 8) -> 256+77.
	// maxStackSize 1 (vehicles don't stack in Beta 1.7.3). Places via its OWN
	// 5.0-block onItemRightClick raycast (item_has_air_use below), not the
	// ordinary onItemPlace block-adjacency path.
	// [740c583: Item.java:351]
	ITEM_BOAT = 333,
	ITEM_LEATHER = 334,
	// Beta Item.bucketMilk: ItemBucket id 79, contents -1 -> 256+79. NOT drinkable in
	// b1.7.3 (no ItemFood milk, no status effects); its only right-click behavior
	// is emptying against any block in reach. [740c583: Item.java:353]
	ITEM_MILK_BUCKET = 335,
	ITEM_BRICK = 336,
	ITEM_CLAY_BALL = 337,
	ITEM_REED = 338,
	ITEM_PAPER = 339,
	ITEM_BOOK = 340,
	ITEM_SLIME_BALL = 341, // Beta Item.slimeBall (plain Item id 85 -> 256+85); size-1 slime drop
	// Beta Item.minecartCrate: ItemMinecart id 86 -> 256+86, minecartType 1
	// (chest cart). [740c583: Item.java static initializer]
	ITEM_MINECART_CHEST = 342,
	// Beta Item.minecartPowered: ItemMinecart id 87 -> 256+87, minecartType 2
	// (furnace cart). [740c583: Item.java static initializer]
	ITEM_MINECART_FURNACE = 343,
	ITEM_EGG = 344, // Beta Item.egg (ItemEgg(88) -> 256+88), maxStackSize 16; chicken lay + throwable
	// Beta Item.fishingRod: ItemFishingRod id 90 -> 256+90, max damage 64,
	// max stack 1, icon (5,4). [740c583: Item.java:98 static initializer]
	ITEM_COMPASS = 345, // Beta Item.compass (Item(89) -> 256+89), icon (6,3); static-needle deviation
	ITEM_FISHING_ROD = 346,
	ITEM_CLOCK = 347, // Beta Item.pocketSundial "clock" (Item(91) -> 256+91), icon (6,4); static-dial deviation
	ITEM_GLOWSTONE_DUST = 348,
	ITEM_FISH = 349,
	ITEM_FISH_COOKED = 350,
	ITEM_DYE = 351,
	// Beta Item.bone: plain Item id 96, shiftedIndex 352. Wolf taming consumes it.
	// [740c583: Item.java static initializer]
	ITEM_BONE = 352,
	ITEM_SUGAR = 353,
	ITEM_CAKE = 354, // Beta Item.cake = ItemReed(98, Block.cake), maxStackSize 1; places block 92
	ITEM_BED = 355,
	ITEM_REDSTONE_REPEATER = 356, // Beta Item.redstoneRepeater (ItemReed(100) -> 256+100); places block 93
	// Beta Item.cookie: ItemCookie id 101 -> 256+101, max stack 8 [Item.java:109].
	// items.c already wires items[357] to item_cookie; this constant was
	// missing (FH-10). [740c583: Item.java static initializer]
	ITEM_COOKIE = 357,
	// Beta Item.mapItem: ItemMap id 102, icon (12, 3) [Item.java:110] ->
	// 256+102. ItemMap's ctor calls setMaxStackSize(1). Registration only: the
	// map-rendering/exploration mechanic (ItemMapBase.updateMapData/MapData)
	// is out of this slice's scope (CR-03).
	// [740c583: Item.java static initializer, ItemMap.java]
	ITEM_MAP = 358,
	ITEM_SHEARS = 359, // Beta's own sequential item ids END HERE -- no id 360 in
						// b1.7.3 [740c583 Item.java static initializer].
	// NET-NEW (non-Beta, FH-08 relocation: docs/fork_hack_audit_2026-08-21.md).
	// This fork's infinite-use zombie-spawn egg (item_egg_zombie.c) inherited
	// id 360 verbatim from fCavEX, squatting the very next id Beta's own
	// sequence would have used -- and the id the REAL game went on to assign
	// to "Melon" once its numeric item ids kept growing past Beta 1.7.3 into
	// the 400s, before the 1.13 "flattening" retired numeric item ids for
	// good (general Minecraft-version knowledge, not re-verified against a
	// later-version decompile -- later versions are outside this project's
	// Beta 1.7.3 oracle/scope). A CaveWii client reading a foreign 360 as
	// this egg -- or a foreign reader seeing our egg as their Melon -- is
	// exactly the "breaks MP" collision FH-08 flagged.
	// Relocated to 500, mirroring the FH-07 `BLOCK_IRON_CHEST` 95->200 move:
	// a round number clear of both Beta's own ceiling (359) and every
	// historically-documented post-Beta numeric item id (which topped out
	// in the 400s), with a wide, deliberately-empty gap short of Beta's own
	// record ids below. No save migration needed -- see items.c for why
	// (the item has never been obtainable in any released build, so no
	// legacy save can hold one under the old id).
	ITEM_EGG_ZOMBIE = 500,
	// Beta music records (creeper-killed-by-skeleton drop; jukebox is S3).
	ITEM_RECORD_13 = 2256,
	ITEM_RECORD_CAT = 2257,
};

#include "../block/blocks_data.h"
#include "../item/tool.h"
struct server_local;
/* S1-a1b: the acting actor for onItemPlace / onUseOnBlock below. */
struct server_player;

enum armor_type {
	ARMOR_TYPE_HELMET = 0,
	ARMOR_TYPE_CHESTPLATE = 1,
	ARMOR_TYPE_LEGGINGS = 2,
	ARMOR_TYPE_BOOTS = 3,
};

enum armor_tier {
	ARMOR_TIER_BLOCK,
	ARMOR_TIER_CHAIN,
	ARMOR_TIER_CLOTH,
	ARMOR_TIER_GOLD,
	ARMOR_TIER_IRON,
	ARMOR_TIER_DIAMOND,
};

enum render_item_env {
	R_ITEM_ENV_INVENTORY,
	R_ITEM_ENV_FIRSTPERSON,
	R_ITEM_ENV_THIRDPERSON,
	R_ITEM_ENV_ENTITY,
	// Mob held item (Beta RenderBiped.renderEquippedItems): the CALLER's view
	// matrix already carries the whole arm/hand transform chain, so the env
	// applies no model transform of its own. Leaves lighting ON afterwards
	// (mid-entity-pass, same GX-descriptor rule as R_ITEM_ENV_ENTITY).
	R_ITEM_ENV_MOB_HAND,
};

struct item {
	char name[32];
	bool has_damage;
	bool has_subtypes; // Beta Item.getHasSubtypes, independent of tool wear
	// Beta's ItemStack maxDamage, verbatim. Until 2026-07-29 all 25 tool/sword entries held
	// Beta's maxDamage PLUS ONE (60/132/251/1562/33), which let every tool last one extra
	// swing and made the durability bar's denominator one too large -- handoff P0(d).
	//
	// ⚠ DO NOT restate this as "maxUses vs maxDamage differ by one". They do NOT, and that
	// framing is how the wrong values got here. Beta's EnumToolMaterial field is *named*
	// maxUses but is fed STRAIGHT into setMaxDamage, so in Beta maxUses == maxDamage:
	//   EnumToolMaterial.java:4-8  WOOD(0, 59) STONE(1, 131) IRON(2, 250) EMERALD(3, 1561)
	//                              GOLD(0, 32)
	//   ItemTool.java:14, ItemSword.java:9, ItemHoe.java:6  setMaxDamage(var.getMaxUses())
	// The only thing that is one higher is the FOLKLORE number people quote ("a wood tool
	// gets 60 uses"), and that is an observed consequence of the strict `>` break test, not
	// a field anywhere in Beta.
	//
	// WORKED EXAMPLE, so nobody "corrects" this back: an iron sword has max_damage 250, and
	// ItemStack.damageItem accumulates then breaks once damage exceeds max damage (ItemStack.java:127). Use 250
	// leaves damage 250, which is NOT > 250 -- it survives. Use 251 leaves 251, which IS
	// > 250 -- that swing destroys it. So: 250 survivable uses, destroyed on the 251st.
	// Note the break tests an ACCUMULATED TOTAL, never a decrementing "uses remaining"
	// counter -- a 2-point hit (ItemSword.onBlockDestroyed, ItemTool.hitEntity) must be able
	// to overshoot the limit rather than be clamped.
	//
	// Flint & steel (64, item_flint_steel.c) and shears (238, items_object.h) are NOT part of
	// that family: their classes pass a literal to setMaxDamage rather than an
	// EnumToolMaterial, so those two already held Beta's maxDamage and must be LEFT ALONE.
	uint16_t max_damage;
	uint8_t max_stack;
	uint8_t fuel;
	// Beta Item.containerItem (setContainerItem): item id left behind IN THE
	// CRAFTING GRID slot after this item is consumed by a craft -- Beta
	// SlotCrafting.onPickupFromSlot replaces the decremented slot with
	// a fresh stack of the container item, so 3 milk buckets crafted into a cake
	// become 3 EMPTY buckets still sitting in the grid. 0 = none. Beta sets it
	// on all three buckets (water/lava/milk -> bucketEmpty). Beta's
	// setContainerItem throws if maxStackSize > 1, so carriers are max_stack 1.
	// [740c583: Item.setContainerItem, SlotCrafting.onPickupFromSlot]
	uint16_t container_item;
	void (*renderItem)(struct item*, struct item_data*, mat4, bool,
					   enum render_item_env);
	// S1-a1b: `actor` is the player whose click this is -- the ONLY body,
	// inventory, facing and health this hook may read or write. Never
	// s->player (that is the HOST on a split-screen server).
	bool (*onItemPlace)(struct server_local*, struct server_player* actor,
						struct item_data*,
						struct block_info*, struct block_info*, enum side);
	// Beta Item.onItemUse: act on the CLICKED block (e.g. bonemeal a crop, hoe-till
	// dirt, flint+steel a face) -- distinct from onItemPlace, which places a block in
	// the adjacent air. Called BEFORE the place dispatch; return true to consume one and
	// suppress placement. `on` is the clicked block_info.
	bool (*onUseOnBlock)(struct server_local*, struct server_player* actor,
						 struct item_data*,
						 struct block_info* on, enum side);
	// Beta Item.onCreated(ItemStack, World, EntityPlayer) [740c583]: fired
	// ONCE, on the stack about to be picked up from the crafting OUTPUT slot
	// -- ItemStack.onCrafting calls it before the picked-up stack is placed
	// into the cursor/inventory (SlotCrafting.onPickupFromSlot,
	// inventory_crafting.c/inventory_player.c's `crafted` branches). May
	// mutate `*stack` in place (ItemMap uses this to stamp a fresh map id
	// into the durability/damage field before the player ever sees it).
	// Beta's parallel craft-statistic increment on the crafting player
	// in the same method is NOT wired -- this engine has no stats/
	// achievement system anywhere (zero StatList/addStat call sites
	// tree-wide; server_local.c:8600 is the standing precedent for this
	// exact gap on the fishing-slice's own addStat call).
	//
	// Return value (NOT in Beta -- this port's own extension, matching the
	// onItemPlace/onUseOnBlock true-to-proceed convention above): true iff
	// `*stack` is now valid and the craft may complete; false iff the hook
	// could not durably finish whatever it needed to (ItemMap: the map id
	// could not be persisted to idcounts.dat, map_idcounts.c) and the craft
	// must be FAILED outright rather than hand out a stack with broken/
	// colliding backing state. Every current call site (inventory_crafting.c
	// /inventory_player.c's CRAFTING_SLOT_OUTPUT branch, server_local.c's
	// map_demo debug rig) checks this and aborts the pickup/give on false.
	// S1 (R-S1a1b F3): `actor` is the ACTING crafter's body -- the acting
	// actor whose inventory the craft came out of, which since S1-a1b can be
	// the split guest (the guest's workbench window is admitted, and the map
	// recipe is 3x3, so it is reachable). It is Beta's crafting EntityPlayer
	// parameter, and ItemMap.onCreated stamps that player's position into the
	// fresh map -- stamping the host's would centre a guest-crafted map on
	// someone else. NULL only where an inventory carries no owner (the
	// client-side prediction mirrors, which never reach the server leg, and
	// harness inventories built with a NULL player); a hook that needs a
	// position must fall back to the host there.
	bool (*onCreated)(struct server_local*, struct server_player* actor,
					  struct item_data* stack);
	// Beta Item.onUpdate(ItemStack, World, Entity, int slot, boolean
	// isCurrentItem) [740c583: ItemStack.updateAnimation]: ticked for EVERY
	// non-empty stack in the player's hotbar+main inventory (NOT armor/
	// crafting/output), every server tick that the player is live
	// (InventoryPlayer.decrementAnimations, called from EntityPlayer.
	// onLivingUpdate). `slot` is the ABSOLUTE index into that stack's owning
	// inventory (INVENTORY_SLOT_MAIN/_HOTBAR-relative, matching how callers
	// already compute "the held slot" elsewhere in server_local.c);
	// `is_held` mirrors Beta's isCurrentItem (held index equals slot,
	// hotbar-relative comparison done by the caller).
	void (*onUpdate)(struct server_local*, struct item_data* stack,
					 size_t slot, bool is_held);
	// Actor-explicit authoritative update. The legacy hook above is kept for
	// single-player callers; dispatch prefers this hook for either local body.
	void (*onUpdateFor)(struct server_local*, struct server_player* actor,
						struct item_data* stack, size_t slot, bool is_held);
	struct item_armor {
		bool is_armor;
		enum armor_type type;
		enum armor_tier tier;
	} armor;
	struct item_tool {
		enum tool_type type;
		enum tool_tier tier;
	} tool;
	union {
		struct {
			uint8_t texture_x : 4;
			uint8_t texture_y : 4;
		} item;
		struct {
			bool has_default;
			uint8_t default_metadata : 4;
			uint8_t default_rotation : 2;
		} block;
	} render_data;
};

#endif
