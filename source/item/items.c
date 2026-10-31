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

#include <assert.h>

#include "../block/blocks.h"
#include "../graphics/render_item.h"
#include "items_object.h"
#include "../network/server_local.h"

struct item* items[ITEMS_MAX];

void items_init() {
	for(int k = 0; k < ITEMS_MAX; k++)
		items[k] = NULL;

	render_item_init();

	for(int k = 0; k < 256; k++) {
		if(blocks[k]) {
			struct item* it = &blocks[k]->block_item;
			strncpy(it->name, blocks[k]->name, sizeof(it->name));
			it->name[sizeof(it->name) - 1] = 0;
			items[k] = it;
		}
	}

	items[256] = &item_iron_shovel;
	items[257] = &item_iron_pickaxe;
	items[258] = &item_iron_axe;
	items[259] = &item_flint_steel;
	items[260] = &item_apple;
	items[261] = &item_bow;
	items[262] = &item_arrow;
	items[263] = &item_coal;
	items[264] = &item_diamond;
	items[265] = &item_iron;
	items[266] = &item_gold;
	items[267] = &item_iron_sword;
	items[268] = &item_wood_sword;
	items[269] = &item_wood_shovel;
	items[270] = &item_wood_pickaxe;
	items[271] = &item_wood_axe;
	items[272] = &item_stone_sword;
	items[273] = &item_stone_shovel;
	items[274] = &item_stone_pickaxe;
	items[275] = &item_stone_axe;
	items[276] = &item_diamond_sword;
	items[277] = &item_diamond_shovel;
	items[278] = &item_diamond_pickaxe;
	items[279] = &item_diamond_axe;
	items[280] = &item_stick;
	items[281] = &item_bowl;
	items[282] = &item_mushroom_stew;
	items[283] = &item_gold_sword;
	items[284] = &item_gold_shovel;
	items[285] = &item_gold_pickaxe;
	items[286] = &item_gold_axe;
	items[287] = &item_string;
	items[288] = &item_feather;
	items[289] = &item_gunpowder;
	items[290] = &item_wood_hoe;
	items[291] = &item_stone_hoe;
	items[292] = &item_iron_hoe;
	items[293] = &item_diamond_hoe;
	items[294] = &item_gold_hoe;
	items[295] = &item_seeds;
	items[296] = &item_wheat;
	items[297] = &item_bread;
	items[298] = &item_leather_helmet;
	items[299] = &item_leather_chestplate;
	items[300] = &item_leather_leggings;
	items[301] = &item_leather_boots;
	items[302] = &item_chain_helmet;
	items[303] = &item_chain_chestplate;
	items[304] = &item_chain_leggings;
	items[305] = &item_chain_boots;
	items[306] = &item_iron_helmet;
	items[307] = &item_iron_chestplate;
	items[308] = &item_iron_leggings;
	items[309] = &item_iron_boots;
	items[310] = &item_diamond_helmet;
	items[311] = &item_diamond_chestplate;
	items[312] = &item_diamond_leggings;
	items[313] = &item_diamond_boots;
	items[314] = &item_gold_helmet;
	items[315] = &item_gold_chestplate;
	items[316] = &item_gold_leggings;
	items[317] = &item_gold_boots;
	items[318] = &item_flint;
	items[319] = &item_porkchop;
	items[320] = &item_porkchop_cooked;
	// CR-03: painting/sign/map were valid Beta ids that stayed unregistered,
	// so item_get() returned NULL for a valid save's item and merge paths in
	// inventory.c dereferenced that NULL for max_stack. Painting (painting-
	// slice, gap audit #9) and sign (wave 3, in-world sign text) are now both
	// fully wired -- real placement, not just registration+safe-stacking; see
	// item_painting.c/entity_painting.c and the sign_text_store slice. Map
	// (ItemMap/MapData, gap audit #9's item #9) remains deferred -- see
	// items_object.h's item_map comment.
	items[321] = &item_painting;
	items[322] = &item_apple_golden;
	items[323] = &item_sign;
	items[324] = &item_door_wood;
	items[325] = &item_bucket;
	items[326] = &item_bucket_water;
	items[327] = &item_bucket_lava;
	items[328] = &item_minecart;
	items[329] = &item_saddle;
	items[330] = &item_door_iron;
	items[331] = &item_redstone;
	items[332] = &item_snow_ball;
	items[333] = &item_boat;
	items[334] = &item_leather;
	items[335] = &item_bucket_milk;
	items[336] = &item_brick;
	items[337] = &item_clay_ball;
	items[338] = &item_sugarcane;
	items[339] = &item_paper;
	items[340] = &item_book;
	items[341] = &item_slime_ball;
	items[342] = &item_minecart_chest;
	items[343] = &item_minecart_furnace;
	items[344] = &item_egg;
	items[345] = &item_compass;
	items[346] = &item_fishing_rod;
	items[347] = &item_clock;
	items[348] = &item_glowstone_dust;
	items[349] = &item_fish;
	items[350] = &item_fish_cooked;
	items[351] = &item_dye;
	items[352] = &item_bone;
	items[353] = &item_sugar;
	items[354] = &item_cake;
	items[355] = &item_bed;
	items[356] = &item_repeater;
	items[357] = &item_cookie;
	items[358] = &item_map;
	items[359] = &item_shears;

	/* [740c583 Item{Sapling,Log,Leaves,Slab,Cloth,Coal,Dye} constructors]
	 * ItemMap and worn tools do NOT have subtypes in Beta 1.7.3. */
	const uint16_t subtypes[] = {6, 17, 18, 44, 35, 263, 351};
	for(size_t i = 0; i < sizeof(subtypes) / sizeof(*subtypes); i++)
		if(items[subtypes[i]]) items[subtypes[i]]->has_subtypes = true;

	// NET-NEW (non-Beta, FH-08 relocation, see items.h ITEM_EGG_ZOMBIE): this
	// fork's zombie-spawn egg used to squat id 360 (the id right after Beta's
	// own ceiling, and the id the real game later gave "Melon"). Deliberately
	// registered by its enum, not a literal, so it reads as "not part of
	// Beta's sequence" the way the literal 256..359 run above does. No save
	// migration required -- the item has never been reachable in any released
	// build (no recipe, no debug/creative kit entry, no mob drop, no give
	// command; verified by a full-repo grep before this relocation landed),
	// so no legacy CaveWii save can contain an id-360 stack under this
	// definition. items[360] is intentionally left NULL: an id-360 stack
	// imported from a foreign (non-Beta) save now correctly resolves to
	// "unknown item" (item_get() -> NULL) instead of silently becoming an
	// infinite zombie-spawner.
	items[ITEM_EGG_ZOMBIE] = &item_egg_zombie;

	// Beta music records (the stubs above were these two all along):
	items[ITEM_RECORD_13] = &item_record_13;
	items[ITEM_RECORD_CAT] = &item_record_cat;
}

struct item* item_get(struct item_data* item) {
	assert(item);
	return item->id < ITEMS_MAX ? items[item->id] : NULL;
}

bool item_is_block(struct item_data* item) {
	assert(item);
	return item_get(item) && item->id < 256 && blocks[item->id];
}

// See the rule table + decompile citations in items.h. Both take the item DEFINITION, not
// the stack, so they stay pure: the caller decides what to do with the amount.
uint16_t item_wear_on_block_break(uint16_t item_id, const struct item* definition,
								  uint8_t broken_block) {
	if(!definition || !definition->has_damage || definition->max_damage == 0)
		return 0;
	switch(definition->tool.type) {
		case TOOL_TYPE_PICKAXE:
		case TOOL_TYPE_SHOVEL:
		case TOOL_TYPE_AXE: return 1;  // ItemTool.onBlockDestroyed
		case TOOL_TYPE_SWORD: return 2; // ItemSword.onBlockDestroyed
		default: break;
	}
	// ItemShears is a plain Item (TOOL_TYPE_ANY here), so it is matched by id, and it is the
	// ONLY item whose wear depends on WHAT was broken. Beta ItemShears.java:10-13:
	//   if (var2 == Block.leaves.blockID || var2 == Block.web.blockID) var1.damageItem(1, var6);
	// Both arms are reachable here -- BLOCK_COBWEB is Beta's Block.web (blocks_data.h:85).
	// Note WOOL is deliberately absent: shears cut wool faster (ItemShears.getStrVsBlock)
	// but that costs no durability in 1.7.3.
	if(item_id == ITEM_SHEARS
	   && (broken_block == BLOCK_LEAVES || broken_block == BLOCK_COBWEB))
		return 1;
	// Everything else -- hoes (ItemHoe overrides nothing), flint & steel, bows, plain
	// items -- does not wear on a block break.
	return 0;
}

uint16_t item_wear_on_hit_entity(const struct item* definition) {
	if(!definition || !definition->has_damage || definition->max_damage == 0)
		return 0;
	switch(definition->tool.type) {
		case TOOL_TYPE_PICKAXE:
		case TOOL_TYPE_SHOVEL:
		case TOOL_TYPE_AXE: return 2;  // ItemTool.hitEntity -- note the INVERSION
		case TOOL_TYPE_SWORD: return 1; // ItemSword.hitEntity
		default: return 0;              // shears/hoe/flint/bow: Item.hitEntity -> false
	}
}

bool item_stack_damage(struct item_data* stack, uint16_t amount) {
	assert(stack);
	struct item* definition = item_get(stack);
	if(!definition || !definition->has_damage || definition->max_damage == 0
	   || stack->count == 0 || amount == 0)
		return false;

	uint32_t damaged = (uint32_t)stack->durability + amount;
	if(damaged <= definition->max_damage) {
		stack->durability = (uint16_t)damaged;
		return false;
	}

	// Beta decrements one stack item and resets itemDamage. Damageable items
	// normally stack to one; normalize an exhausted CaveWii slot completely so
	// no count-zero ghost item survives inventory serialization/rendering.
	stack->count--;
	stack->durability = 0;
	if(stack->count == 0)
		*stack = (struct item_data) {0};
	return true;
}


