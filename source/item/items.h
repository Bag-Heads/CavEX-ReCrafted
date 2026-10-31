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

#ifndef ITEMS_H
#define ITEMS_H

// Value types live in item_types.h (step 7 split); this header is the
// item API + registry. Including items.h still provides everything it
// always did.
#include "item_types.h"

// 2304, not 512: Beta's music records live at ids 2256/2257. The registry is
// a pointer array, so the widening costs ~7KB of BSS -- cheap for keeping
// Beta-compatible record ids in saves.
#define ITEMS_MAX 2304

extern struct item* items[ITEMS_MAX];

void items_init(void);
struct item* item_get(struct item_data* item);
bool item_is_block(struct item_data* item);
// M4 S5: does this held item DO something on a right-click into open air (no
// block, no entity under the crosshair)? The client's open-air IB_ACTION2
// branch gates SRPC_USE_ITEM on this, and the server dispatches per held id.
// The smallest faithful mechanism for the current air verbs -- the bow
// (audit #17c instant fire), the thrown egg/snowball (Beta
// ItemEgg.onItemRightClick / ItemSnowball.onItemRightClick fire anywhere; the
// at-a-block half rides the SRPC_BLOCK_PLACE fall-through), the fishing
// rod (fishing-slice: ItemFishingRod.onItemRightClick is the SAME kind of
// hook -- fires anywhere, cast or reel decided by fishEntity==null -- so it
// gets the SAME open-air path + block-place fall-through as egg/snowball;
// unlike the bow, there is no controller-ambiguity reason to keep it
// open-air-only, and fishing genuinely needs the at-a-block case: aiming at
// the shoreline is common, not the exception), and the boat.
//
// BOAT-SLICE ADDITION: ItemBoat.onItemRightClick ALSO always does its own
// independent 5.0-block raycast (with stopOnLiquid=true, unlike the ordinary
// block-highlight raycast) [740c583: ItemBoat.onItemRightClick] -- it never
// consults the normal targeted-block result at all. Gating it here (fires
// only when nothing is targeted the ordinary way) is the SAME accepted
// simplification already governing bow/egg/snowball: a solid block within
// normal reach takes priority over the item's own independent-target action.
// Water itself is never a "targeted block" here (block_water.c's
// getBoundingBox returns 0 unconditionally, so the ordinary raycast passes
// through it), so aiming at open/near water is exactly the common case this
// gate already serves. See BOAT_SLICE_NOTES.md.
static inline bool item_has_air_use(uint16_t id) {
	return id == ITEM_BOW || id == ITEM_EGG || id == ITEM_SNOW_BALL
		|| id == ITEM_FISHING_ROD || id == ITEM_BOAT;
}
/* Beta 1.7.3 ItemFood (ItemFood.onItemRightClick :14-17: --stackSize;
 * player.heal(healAmount)) -- eating is INSTANT, no use duration -- and its
 * subclass ItemSoup (mushroom stew -> bowl). Item.java registers exactly these
 * nine: apple, mushroomStew, bread, porkRaw, porkCooked, appleGold, fishRaw,
 * fishCooked, cookie. They are eaten from Minecraft.clickMouse's trailing
 * sendUseItem (:834-838), so a right-click with NO block target eats too. */
static inline bool item_is_food(uint16_t id) {
	return id == ITEM_APPLE || id == ITEM_MUSHROOM_STEW || id == ITEM_BREAD
		|| id == ITEM_PORKCHOP || id == ITEM_PORKCHOP_COOKED
		|| id == ITEM_APPLE_GOLDEN || id == ITEM_FISH
		|| id == ITEM_FISH_COOKED || id == ITEM_COOKIE;
}
// Beta ItemStack.damageItem: add damage, then break one item only when the
// accumulated value is strictly greater than max_damage (maxDamage=64 lasts
// 65 uses). Returns true when an item broke.
bool item_stack_damage(struct item_data* stack, uint16_t amount);

/* WEAR AMOUNTS -- how much durability one block-break / one melee hit costs the held item.
   0 means "this item does not wear on this path". Pure, so the whole rule table is testable
   without a world.

   The rule is NOT "anything with has_damage wears". In Beta only ItemTool and ItemSword
   override onBlockDestroyed/hitEntity; Item.java:197-203 returns false for both, so plain
   items never wear. CaveWii marks hoes, shears, flint & steel and bows has_damage = true,
   so gating on that field would wrongly wear a hoe on every dig -- gate on tool.type (plus
   the shears id), which is what these do.

   Verified verbatim at 740c583:
     ItemTool.java:34-36   onBlockDestroyed -> damageItem(1)   -- NO hardness gate. The
                           `getHardness() != 0` guard is a LATER version: in 1.7.3 breaking
                           a torch with a pickaxe costs 1.
     ItemTool.java:29-31   hitEntity        -> damageItem(2)
     ItemSword.java:22-24  onBlockDestroyed -> damageItem(2)
     ItemSword.java:17-19  hitEntity        -> damageItem(1)   -- INVERTED vs the break path
     ItemShears.java:10-15 onBlockDestroyed -> damageItem(1) ONLY for leaves or web
     ItemHoe.java          extends Item, overrides NEITHER -- hoes never wear this way
     ItemFlintAndSteel     overrides neither -- it wears only through its own onItemUse */
uint16_t item_wear_on_block_break(uint16_t item_id, const struct item* definition,
								  uint8_t broken_block);
uint16_t item_wear_on_hit_entity(const struct item* definition);
/* Exact ItemMinecart constructor coordinate, shared with its placement golden. */
void item_minecart_spawn_position(const struct block_info* rail, vec3 out);

#endif
