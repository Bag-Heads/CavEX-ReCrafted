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

#include "items.h"

// Sign-slice: item_sign's onItemPlace needs both BlockSign ids,
// block_beta_material_is_solid, the shared block_sign placement helper, and
// server_local's player yaw (block_sign.c / server_local.h). blocks.h already
// includes items.h itself, and all three headers are include-guarded.
#include "../block/blocks.h"
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h) -- this header's onItemPlace/onUseOnBlock bodies dereference struct server_local fields directly (e.g. s->player.rx)

static struct item item_iron_sword = {
	.name = "Iron Sword",
	.has_damage = true,
	.max_damage = 250, // Beta EnumToolMaterial IRON maxUses; 250 survivable uses, 251st breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_SWORD,
	.tool.tier = TOOL_TIER_IRON,
	.render_data = {
		.item = {
			.texture_x = 2,
			.texture_y = 4,
		},
	},
};

static struct item item_iron_shovel = {
	.name = "Iron Shovel",
	.has_damage = true,
	.max_damage = 250, // Beta EnumToolMaterial IRON maxUses; 250 survivable uses, 251st breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_SHOVEL,
	.tool.tier = TOOL_TIER_IRON,
	.render_data = {
		.item = {
			.texture_x = 2,
			.texture_y = 5,
		},
	},
};

static struct item item_iron_pickaxe = {
	.name = "Iron Pickaxe",
	.has_damage = true,
	.max_damage = 250, // Beta EnumToolMaterial IRON maxUses; 250 survivable uses, 251st breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_PICKAXE,
	.tool.tier = TOOL_TIER_IRON,
	.render_data = {
		.item = {
			.texture_x = 2,
			.texture_y = 6,
		},
	},
};

static struct item item_iron_axe = {
	.name = "Iron Axe",
	.has_damage = true,
	.max_damage = 250, // Beta EnumToolMaterial IRON maxUses; 250 survivable uses, 251st breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_AXE,
	.tool.tier = TOOL_TIER_IRON,
	.render_data = {
		.item = {
			.texture_x = 2,
			.texture_y = 7,
		},
	},
};

// Shared bone-meal-style use-on-block for all hoes (source/item/items/item_hoe.c):
// tills grass/dirt into farmland.
extern bool item_hoe_onUseOnBlock(struct server_local*, struct server_player* actor, struct item_data*,
								  struct block_info*, enum side);

static struct item item_iron_hoe = {
	.name = "Iron Hoe",
	.has_damage = true,
	.max_damage = 250, // Beta EnumToolMaterial IRON maxUses; 250 survivable uses, 251st breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_HOE,
	.tool.tier = TOOL_TIER_IRON,
	.onUseOnBlock = item_hoe_onUseOnBlock,
	.render_data = {
		.item = {
			.texture_x = 2,
			.texture_y = 8,
		},
	},
};

static struct item item_wood_sword = {
	.name = "Wooden Sword",
	.has_damage = true,
	.max_damage = 59, // Beta EnumToolMaterial WOOD maxUses; 59 survivable uses, 60th breaks
	.max_stack = 1,
	.fuel = 2,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_SWORD,
	.tool.tier = TOOL_TIER_WOOD,
	.render_data = {
		.item = {
			.texture_x = 0,
			.texture_y = 4,
		},
	},
};

static struct item item_wood_shovel = {
	.name = "Wooden Shovel",
	.has_damage = true,
	.max_damage = 59, // Beta EnumToolMaterial WOOD maxUses; 59 survivable uses, 60th breaks
	.max_stack = 1,
	.fuel = 2,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_SHOVEL,
	.tool.tier = TOOL_TIER_WOOD,
	.render_data = {
		.item = {
			.texture_x = 0,
			.texture_y = 5,
		},
	},
};

static struct item item_wood_pickaxe = {
	.name = "Wooden Pickaxe",
	.has_damage = true,
	.max_damage = 59, // Beta EnumToolMaterial WOOD maxUses; 59 survivable uses, 60th breaks
	.max_stack = 1,
	.fuel = 2,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_PICKAXE,
	.tool.tier = TOOL_TIER_WOOD,
	.render_data = {
		.item = {
			.texture_x = 0,
			.texture_y = 6,
		},
	},
};

static struct item item_wood_axe = {
	.name = "Wooden Axe",
	.has_damage = true,
	.max_damage = 59, // Beta EnumToolMaterial WOOD maxUses; 59 survivable uses, 60th breaks
	.max_stack = 1,
	.fuel = 2,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_AXE,
	.tool.tier = TOOL_TIER_WOOD,
	.render_data = {
		.item = {
			.texture_x = 0,
			.texture_y = 7,
		},
	},
};

static struct item item_wood_hoe = {
	.name = "Wooden Hoe",
	.has_damage = true,
	.max_damage = 59, // Beta EnumToolMaterial WOOD maxUses; 59 survivable uses, 60th breaks
	.max_stack = 1,
	.fuel = 2,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_HOE,
	.tool.tier = TOOL_TIER_WOOD,
	.onUseOnBlock = item_hoe_onUseOnBlock,
	.render_data = {
		.item = {
			.texture_x = 0,
			.texture_y = 8,
		},
	},
};

static struct item item_stone_sword = {
	.name = "Stone Sword",
	.has_damage = true,
	.max_damage = 131, // Beta EnumToolMaterial STONE maxUses; 131 survivable uses, 132nd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_SWORD,
	.tool.tier = TOOL_TIER_STONE,
	.render_data = {
		.item = {
			.texture_x = 1,
			.texture_y = 4,
		},
	},
};

static struct item item_stone_shovel = {
	.name = "Stone Shovel",
	.has_damage = true,
	.max_damage = 131, // Beta EnumToolMaterial STONE maxUses; 131 survivable uses, 132nd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_SHOVEL,
	.tool.tier = TOOL_TIER_STONE,
	.render_data = {
		.item = {
			.texture_x = 1,
			.texture_y = 5,
		},
	},
};

static struct item item_stone_pickaxe = {
	.name = "Stone Pickaxe",
	.has_damage = true,
	.max_damage = 131, // Beta EnumToolMaterial STONE maxUses; 131 survivable uses, 132nd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_PICKAXE,
	.tool.tier = TOOL_TIER_STONE,
	.render_data = {
		.item = {
			.texture_x = 1,
			.texture_y = 6,
		},
	},
};

static struct item item_stone_axe = {
	.name = "Stone Axe",
	.has_damage = true,
	.max_damage = 131, // Beta EnumToolMaterial STONE maxUses; 131 survivable uses, 132nd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_AXE,
	.tool.tier = TOOL_TIER_STONE,
	.render_data = {
		.item = {
			.texture_x = 1,
			.texture_y = 7,
		},
	},
};

static struct item item_stone_hoe = {
	.name = "Stone Hoe",
	.has_damage = true,
	.max_damage = 131, // Beta EnumToolMaterial STONE maxUses; 131 survivable uses, 132nd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_HOE,
	.tool.tier = TOOL_TIER_STONE,
	.onUseOnBlock = item_hoe_onUseOnBlock,
	.render_data = {
		.item = {
			.texture_x = 1,
			.texture_y = 8,
		},
	},
};

static struct item item_diamond_sword = {
	.name = "Diamond Sword",
	.has_damage = true,
	.max_damage = 1561, // Beta EnumToolMaterial EMERALD maxUses; 1561 survivable, 1562nd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_SWORD,
	.tool.tier = TOOL_TIER_DIAMOND,
	.render_data = {
		.item = {
			.texture_x = 3,
			.texture_y = 4,
		},
	},
};

static struct item item_diamond_shovel = {
	.name = "Diamond Shovel",
	.has_damage = true,
	.max_damage = 1561, // Beta EnumToolMaterial EMERALD maxUses; 1561 survivable, 1562nd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_SHOVEL,
	.tool.tier = TOOL_TIER_DIAMOND,
	.render_data = {
		.item = {
			.texture_x = 3,
			.texture_y = 5,
		},
	},
};

static struct item item_diamond_pickaxe = {
	.name = "Diamond Pickaxe",
	.has_damage = true,
	.max_damage = 1561, // Beta EnumToolMaterial EMERALD maxUses; 1561 survivable, 1562nd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_PICKAXE,
	.tool.tier = TOOL_TIER_DIAMOND,
	.render_data = {
		.item = {
			.texture_x = 3,
			.texture_y = 6,
		},
	},
};

static struct item item_diamond_axe = {
	.name = "Diamond Axe",
	.has_damage = true,
	.max_damage = 1561, // Beta EnumToolMaterial EMERALD maxUses; 1561 survivable, 1562nd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_AXE,
	.tool.tier = TOOL_TIER_DIAMOND,
	.render_data = {
		.item = {
			.texture_x = 3,
			.texture_y = 7,
		},
	},
};

static struct item item_diamond_hoe = {
	.name = "Diamond Hoe",
	.has_damage = true,
	.max_damage = 1561, // Beta EnumToolMaterial EMERALD maxUses; 1561 survivable, 1562nd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_HOE,
	.tool.tier = TOOL_TIER_DIAMOND,
	.onUseOnBlock = item_hoe_onUseOnBlock,
	.render_data = {
		.item = {
			.texture_x = 3,
			.texture_y = 8,
		},
	},
};

static struct item item_gold_sword = {
	.name = "Gold Sword",
	.has_damage = true,
	.max_damage = 32, // Beta EnumToolMaterial GOLD maxUses; 32 survivable uses, 33rd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_SWORD,
	.tool.tier = TOOL_TIER_GOLD,
	.render_data = {
		.item = {
			.texture_x = 4,
			.texture_y = 4,
		},
	},
};

static struct item item_gold_shovel = {
	.name = "Gold Shovel",
	.has_damage = true,
	.max_damage = 32, // Beta EnumToolMaterial GOLD maxUses; 32 survivable uses, 33rd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_SHOVEL,
	.tool.tier = TOOL_TIER_GOLD,
	.render_data = {
		.item = {
			.texture_x = 4,
			.texture_y = 5,
		},
	},
};

static struct item item_gold_pickaxe = {
	.name = "Gold Pickaxe",
	.has_damage = true,
	.max_damage = 32, // Beta EnumToolMaterial GOLD maxUses; 32 survivable uses, 33rd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_PICKAXE,
	.tool.tier = TOOL_TIER_GOLD,
	.render_data = {
		.item = {
			.texture_x = 4,
			.texture_y = 6,
		},
	},
};

static struct item item_gold_axe = {
	.name = "Gold Axe",
	.has_damage = true,
	.max_damage = 32, // Beta EnumToolMaterial GOLD maxUses; 32 survivable uses, 33rd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_AXE,
	.tool.tier = TOOL_TIER_GOLD,
	.render_data = {
		.item = {
			.texture_x = 4,
			.texture_y = 7,
		},
	},
};

static struct item item_gold_hoe = {
	.name = "Gold Hoe",
	.has_damage = true,
	.max_damage = 32, // Beta EnumToolMaterial GOLD maxUses; 32 survivable uses, 33rd breaks
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_HOE,
	.tool.tier = TOOL_TIER_GOLD,
	.onUseOnBlock = item_hoe_onUseOnBlock,
	.render_data = {
		.item = {
			.texture_x = 4,
			.texture_y = 8,
		},
	},
};

extern struct item item_flint_steel;

extern struct item item_apple;

static struct item item_bow = {
	.name = "Bow",
	.has_damage = false,
	.max_stack = 1,
	.fuel = 1,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 5,
			.texture_y = 1,
		},
	},
};

static struct item item_arrow = {
	.name = "Arrow",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 5,
			.texture_y = 2,
		},
	},
};

static struct item item_coal = {
	.name = "Coal",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 8,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 7,
			.texture_y = 0,
		},
	},
};

static struct item item_diamond = {
	.name = "Diamond",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 7,
			.texture_y = 3,
		},
	},
};

static struct item item_iron = {
	.name = "Iron Ingot",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 7,
			.texture_y = 1,
		},
	},
};

static struct item item_gold = {
	.name = "Gold Ingot",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 7,
			.texture_y = 2,
		},
	},
};

static struct item item_stick = {
	.name = "Stick",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 1,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 5,
			.texture_y = 3,
		},
	},
};

static struct item item_diamond_helmet = {
	.name = "Diamond Helmet",
	.has_damage = true,
	.max_damage = 264, // [740c583: ItemArmor] helmet base 11 * 3 << diamond level 3
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_HELMET,
	.armor.tier = ARMOR_TIER_DIAMOND,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 3,
			.texture_y = 0,
		},
	},
};

static struct item item_diamond_chestplate = {
	.name = "Diamond Chestplate",
	.has_damage = true,
	.max_damage = 384, // [740c583: ItemArmor] chest base 16 * 3 << diamond level 3
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_CHESTPLATE,
	.armor.tier = ARMOR_TIER_DIAMOND,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 3,
			.texture_y = 1,
		},
	},
};

static struct item item_diamond_leggings = {
	.name = "Diamond Leggings",
	.has_damage = true,
	.max_damage = 360, // [740c583: ItemArmor] legs base 15 * 3 << diamond level 3
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_LEGGINGS,
	.armor.tier = ARMOR_TIER_DIAMOND,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 3,
			.texture_y = 2,
		},
	},
};

static struct item item_diamond_boots = {
	.name = "Diamond Boots",
	.has_damage = true,
	.max_damage = 312, // [740c583: ItemArmor] boots base 13 * 3 << diamond level 3
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_BOOTS,
	.armor.tier = ARMOR_TIER_DIAMOND,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 3,
			.texture_y = 3,
		},
	},
};

static struct item item_leather_helmet = {
	.name = "Leather Helmet",
	.has_damage = true,
	.max_damage = 33, // [740c583: ItemArmor] helmet base 11 * 3 << leather level 0
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_HELMET,
	.armor.tier = ARMOR_TIER_CLOTH,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 0,
			.texture_y = 0,
		},
	},
};

static struct item item_leather_chestplate = {
	.name = "Leather Chestplate",
	.has_damage = true,
	.max_damage = 48, // [740c583: ItemArmor] chest base 16 * 3 << leather level 0
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_CHESTPLATE,
	.armor.tier = ARMOR_TIER_CLOTH,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 0,
			.texture_y = 1,
		},
	},
};

static struct item item_leather_leggings = {
	.name = "Leather Leggings",
	.has_damage = true,
	.max_damage = 45, // [740c583: ItemArmor] legs base 15 * 3 << leather level 0
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_LEGGINGS,
	.armor.tier = ARMOR_TIER_CLOTH,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 0,
			.texture_y = 2,
		},
	},
};

static struct item item_leather_boots = {
	.name = "Leather Boots",
	.has_damage = true,
	.max_damage = 39, // [740c583: ItemArmor] boots base 13 * 3 << leather level 0
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_BOOTS,
	.armor.tier = ARMOR_TIER_CLOTH,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 0,
			.texture_y = 3,
		},
	},
};

static struct item item_chain_helmet = {
	.name = "Chain Helmet",
	.has_damage = true,
	.max_damage = 66, // [740c583: ItemArmor] helmet base 11 * 3 << chain level 1
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_HELMET,
	.armor.tier = ARMOR_TIER_CHAIN,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 1,
			.texture_y = 0,
		},
	},
};

static struct item item_chain_chestplate = {
	.name = "Chain Chestplate",
	.has_damage = true,
	.max_damage = 96, // [740c583: ItemArmor] chest base 16 * 3 << chain level 1
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_CHESTPLATE,
	.armor.tier = ARMOR_TIER_CHAIN,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 1,
			.texture_y = 1,
		},
	},
};

static struct item item_chain_leggings = {
	.name = "Chain Leggings",
	.has_damage = true,
	.max_damage = 90, // [740c583: ItemArmor] legs base 15 * 3 << chain level 1
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_LEGGINGS,
	.armor.tier = ARMOR_TIER_CHAIN,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 1,
			.texture_y = 2,
		},
	},
};

static struct item item_chain_boots = {
	.name = "Chain Boots",
	.has_damage = true,
	.max_damage = 78, // [740c583: ItemArmor] boots base 13 * 3 << chain level 1
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_BOOTS,
	.armor.tier = ARMOR_TIER_CHAIN,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 1,
			.texture_y = 3,
		},
	},
};

static struct item item_iron_helmet = {
	.name = "Iron Helmet",
	.has_damage = true,
	.max_damage = 132, // [740c583: ItemArmor] helmet base 11 * 3 << iron level 2
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_HELMET,
	.armor.tier = ARMOR_TIER_IRON,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 2,
			.texture_y = 0,
		},
	},
};

static struct item item_iron_chestplate = {
	.name = "Iron Chestplate",
	.has_damage = true,
	.max_damage = 192, // [740c583: ItemArmor] chest base 16 * 3 << iron level 2
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_CHESTPLATE,
	.armor.tier = ARMOR_TIER_IRON,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 2,
			.texture_y = 1,
		},
	},
};

static struct item item_iron_leggings = {
	.name = "Iron Leggings",
	.has_damage = true,
	.max_damage = 180, // [740c583: ItemArmor] legs base 15 * 3 << iron level 2
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_LEGGINGS,
	.armor.tier = ARMOR_TIER_IRON,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 2,
			.texture_y = 2,
		},
	},
};

static struct item item_iron_boots = {
	.name = "Iron Boots",
	.has_damage = true,
	.max_damage = 156, // [740c583: ItemArmor] boots base 13 * 3 << iron level 2
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_BOOTS,
	.armor.tier = ARMOR_TIER_IRON,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 2,
			.texture_y = 3,
		},
	},
};

static struct item item_gold_helmet = {
	.name = "Gold Helmet",
	.has_damage = true,
	.max_damage = 66, // [740c583: ItemArmor] helmet base 11 * 3 << gold level 1
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_HELMET,
	.armor.tier = ARMOR_TIER_GOLD,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 4,
			.texture_y = 0,
		},
	},
};

static struct item item_gold_chestplate = {
	.name = "Gold Chestplate",
	.has_damage = true,
	.max_damage = 96, // [740c583: ItemArmor] chest base 16 * 3 << gold level 1
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_CHESTPLATE,
	.armor.tier = ARMOR_TIER_GOLD,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 4,
			.texture_y = 1,
		},
	},
};

static struct item item_gold_leggings = {
	.name = "Gold Leggings",
	.has_damage = true,
	.max_damage = 90, // [740c583: ItemArmor] legs base 15 * 3 << gold level 1
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_LEGGINGS,
	.armor.tier = ARMOR_TIER_GOLD,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 4,
			.texture_y = 2,
		},
	},
};

static struct item item_gold_boots = {
	.name = "Gold Boots",
	.has_damage = true,
	.max_damage = 78, // [740c583: ItemArmor] boots base 13 * 3 << gold level 1
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = true,
	.armor.type = ARMOR_TYPE_BOOTS,
	.armor.tier = ARMOR_TIER_GOLD,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 4,
			.texture_y = 3,
		},
	},
};

extern struct item item_porkchop;

extern struct item item_porkchop_cooked;

extern struct item item_seeds;

static struct item item_wheat = {
	.name = "Wheat",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 9,
			.texture_y = 1,
		},
	},
};

extern struct item item_bread;

static struct item item_flint = {
	.name = "Flint",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 6,
			.texture_y = 0,
		},
	},
};

static struct item item_string = {
	.name = "String",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 8,
			.texture_y = 0,
		},
	},
};


extern struct item item_redstone;
extern struct item item_repeater;


static struct item item_snow_ball = {
	.name = "Snowball",
	.has_damage = false,
	.max_stack = 16,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 14,
			.texture_y = 0,
		},
	},
};

static struct item item_clay_ball = {
	.name = "Clay",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 9,
			.texture_y = 3,
		},
	},
};

static struct item item_glowstone_dust = {
	.name = "Glowstone Dust",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 9,
			.texture_y = 4,
		},
	},
};

// Beta 1.7.3 compass [740c583: Item.java:363 `(new Item(89)).setIconCoord(6, 3)
// .setItemName("compass")`]: a plain item -- default maxStackSize 64, no
// damage, no use behavior; its gameplay role is the crafting chain (map =
// paper ring + compass, CraftingManager:73). RENDER: TextureCompassFX (the
// live needle animation) is now ported byte-faithfully -- see
// source/graphics/compass_fx.h and the runtime consumer
// source/platform/texture.c's item_needle_fx_tick. Default ON on PC and Wii;
// opt out with gfx.item_needle_anim=0. With the toggle off,
// the icon stays the pack's STATIC items.png
// tile (6,3), same as before this port. Simulation-neutral either way.
static struct item item_compass = {
	.name = "Compass",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 6,
			.texture_y = 3,
		},
	},
};

// Beta 1.7.3 clock [740c583: Item.java:365 `(new Item(91)).setIconCoord(6, 4)
// .setItemName("clock")` -- field name pocketSundial]: plain item like the
// compass. RENDER: TextureWatchFX is now ported byte-faithfully (see
// source/graphics/watch_fx.h), gated the same way as the compass ABOVE this
// entry -- plus an asset gap: Beta's misc/dial.png (the rotated dial-ring
// texture) is Mojang IP and is not bundled, so even with the toggle on, the
// clock keeps the static items.png tile (6,4) until a real Beta pack supplies
// misc/dial.png (texture_dial in source/platform/texture.c).
static struct item item_clock = {
	.name = "Clock",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 6,
			.texture_y = 4,
		},
	},
};

// Moved to source/item/items/item_dye.c (modular) to carry the bone-meal onUseOnBlock
// without pulling engine headers into this monolith.
extern struct item item_dye;

// Beta [decompile 740c583] Item.java:299 `bowlEmpty = (new Item(25))
// .setIconCoord(7,4).setItemName("bowl");` -- a plain Item that never calls
// setMaxStackSize, so it keeps the class default `protected int
// maxStackSize = 64;` (Item.java:115). Was 1 (an undocumented, uncited
// divergence) -- Batch H parity fix, 2026-08-25.
static struct item item_bowl = {
	.name = "Bowl",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 1,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 7,
			.texture_y = 4,
		},
	},
};


extern struct item item_mushroom_stew;

static struct item item_feather = {
	.name = "Feather",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 8,
			.texture_y = 1,
		},
	},
};

static struct item item_gunpowder = {
	.name = "Gunpowder",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 8,
			.texture_y = 2,
		},
	},
};

static struct item item_leather = {
	.name = "Leather",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 7,
			.texture_y = 6,
		},
	},
};

static struct item item_brick = {
	.name = "Brick",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 6,
			.texture_y = 1,
		},
	},
};

static struct item item_paper = {
	.name = "Paper",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 10,
			.texture_y = 3,
		},
	},
};

static struct item item_book = {
	.name = "Book",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 11,
			.texture_y = 3,
		},
	},
};

static struct item item_sugar = {
	.name = "Sugar",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 13,
			.texture_y = 0,
		},
	},
};

// Moved to source/item/items/item_cake.c (the item_dye precedent) to carry the
// BLOCK_CAKE-placing onItemPlace without pulling engine headers into this monolith.
extern struct item item_cake;


extern struct item item_bed;

// Beta 1.7.3 map item, part 2a (Batch F): onCreated/onUpdate dispatch, the
// colour-scan core, and the resident map-data cache now live in
// source/item/items/item_map.c (moved out of this monolith per the
// item_dye/item_cake precedent, since ItemMap needs server_local/map_data/
// map_idcounts engine headers this file deliberately stays free of). See
// item_map.h for the full scope.
extern struct item item_map;

static struct item item_shears = {
	.name = "Shears",
	// M4 S2: Beta ItemShears ctor setMaxDamage(238); item_stack_damage is the
	// consumer (the shear interact applies 1 damage per shear, authoritative
	// side only). Leaf-break dig damage remains deferred with the other tools.
	.has_damage = true,
	.max_damage = 238,
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 13,
			.texture_y = 5,
		},
	},
};


extern struct item item_fish;

extern struct item item_fish_cooked;

// The real struct, with onItemPlace wired to the Beta ItemFood eat/heal path
// (Batch H parity fix, 2026-08-25 -- was a registration-only stub with zero
// use behavior), now lives in source/item/items/item_cookie.c, mirroring
// item_apple.c/item_bread.c's file-per-item convention for anything with
// real onXxx behavior.
extern struct item item_cookie;

// Painting-slice (gap audit #9): the real struct, with onUseOnBlock wired to
// the actual EntityPainting placement path, now lives in
// source/item/items/item_painting.c (mirroring item_boat.c/item_minecart.c's
// own file-per-item convention for anything with real onXxx behavior) --
// this used to be an inert registration-only static struct here (CR-03,
// 2026-08-20); superseded, not merely edited in place, since the definition
// moved TU.
extern struct item item_painting;

extern struct item item_apple_golden;

// Beta 1.7.3 signPost 16-direction rotation from the placing player's yaw
// [740c583 ItemSign.onItemUse]:
//   MathHelper.floor_double((double)((player.rotationYaw + 180.0F) * 16.0F
//                                     / 360.0F) + 0.5D) & 15
// floor_double [740c583 MathHelper.java] is NOT a C (int) truncation -- it
// floors toward negative infinity:
//   int var2 = (int)var0; return var0 < (double)var2 ? var2 - 1 : var2;
// Ported as a pure, directly testable function; the top-face placement path
// below is its production caller and tests/test_sign_placement.c pins the
// formula at cardinal, diagonal, wraparound, and negative-yaw inputs.
static int32_t item_sign_post_direction_from_yaw(float yaw) {
	double v = (double)((yaw + 180.0F) * 16.0F / 360.0F) + 0.5;
	int32_t fl = (int32_t)v;
	return (v < (double)fl ? fl - 1 : fl) & 15;
}

// Beta 1.7.3 ItemSign.onItemUse [740c583], summarized (side = the
// clicked face index; the CLICKED block's position is the start; the
// engine's dispatcher (server_local.c SRPC_BLOCK_PLACE) has already computed
// `where` = that position offset by side exactly like Beta's manual
// per-axis increment/decrement side switch, via the SAME blocks_side_offset table --
// so this function does not need to redo that offset arithmetic):
//
//   side 0 (bottom face) fails;
//   a clicked block whose material is not solid fails;
//   [the position is offset by side -- already done for us as `where`]
//   a target cell where the sign post cannot be placed fails;
//   side 1 places Block.signPost with metadata
//       floor((yaw + 180) * 16 / 360 + 0.5) & 15
//           (float math on rotationYaw, widened to double before the + 0.5);
//   any other side places
//       Block.signWall with the side as metadata;
//   then
//   the stack shrinks by one,
//   the sign tile entity, when present, opens the edit-sign GUI,
//   and the answer is true.
//
// Mapped onto this engine:
//  - side 0 (Beta BOTTOM, -Y)   -> on_side == SIDE_BOTTOM (blocks_side_offset: TOP=+Y, BOTTOM=-Y)
//  - side 1 (Beta TOP, +Y)       -> on_side == SIDE_TOP    -> Block.signPost (id 63)
//  - side 2/3/4/5 (N/S/W/E)      -> on_side == SIDE_FRONT/SIDE_BACK/SIDE_LEFT/SIDE_RIGHT -> Block.signWall (id 68)
//
// --stackSize is NOT done here: onItemPlace returning true is auto-consumed
// by the generic dispatch (server_local.c's SRPC_BLOCK_PLACE `if(placed)
// inventory_consume(...)`), the SAME convention block_cake.c's onItemPlace
// already relies on -- exactly Beta's stack decrement on the success path.
//
// displayGUIEditSign is wired at the DISPATCH level (server_local.c, right
// after this returns true and the item is consumed) via the EXISTING
// server_local_open_inventory(&inventory_logic_sign, ...) call block_sign.c's
// onRightClick already uses to open an existing sign -- no new RPC plumbing,
// so it is wired rather than skipped. See server_local.c's SRPC_BLOCK_PLACE
// handler.
static bool item_sign_onItemPlace(struct server_local* s, struct server_player* actor, struct item_data* it,
								  struct block_info* where, struct block_info* on,
								  enum side on_side) {
	(void)it; // --stackSize handled by the generic dispatch on a true return.
	if(!s || !where || !where->block || !on || !on->block)
		return false;

	// var7 == 0: Beta's BOTTOM face. No sign of either kind on an underside.
	if(on_side == SIDE_BOTTOM)
		return false;

	// !getBlockMaterial(x,y,z).isSolid(): the CLICKED block's material must be
	// solid (refuses e.g. a torch, a flower, water, tall grass as the target face).
	if(!block_beta_material_is_solid(on->block->type))
		return false;

	// Block.signPost.canPlaceBlockAt(world,x,y,z) on the TARGET cell: neither
	// signPost nor signWall override canPlaceBlockAt, so both inherit the base
	// Block.canPlaceBlockAt (target id==0 (air) OR a "ground cover" material).
	// `place_ignore` is this engine's equivalent of that predicate -- the SAME
	// gate the generic SRPC_BLOCK_PLACE dispatch already applies before calling
	// ANY item's onItemPlace (server_local.c), reapplied here so this function
	// stays correct when called directly (as the test does, bypassing the
	// dispatcher).
	if(blocks[where->block->type] && !blocks[where->block->type]->place_ignore)
		return false;

	if(on_side == SIDE_TOP) {
		// var7 == 1: Block.signPost (63), metadata = 0..15 yaw index.
		return block_sign_place(
			s, actor, where, BLOCK_SIGN_POST,
			(uint8_t)item_sign_post_direction_from_yaw(actor->rx));
	}

	// var7 in {2,3,4,5} (Beta N/S/W/E): Block.signWall (id 68), metadata =
	// var7 verbatim, through the enum-side mapping pinned in block_sign.c.
	return block_sign_place(s, actor, where, BLOCK_SIGN,
							block_sign_wall_metadata_for_side(on_side));
}

// CR-03 registration + sign-slice wiring: Beta ItemSign's ctor sets
// maxStackSize = 1 explicitly (signs never stack). [740c583: ItemSign.java]
// Icon: Beta setIconCoord(10, 2). onItemPlace above ports ItemSign.onItemUse
// (top face -> signPost/BLOCK_SIGN_POST with yaw metadata; side faces ->
// signWall/BLOCK_SIGN with Beta's raw facing metadata; bottom face ->
// refused); the post-place edit-GUI open is wired at the dispatch level
// (server_local.c). The sign RECIPE (recipe.c) outputs this item, one at a
// time, matching CraftingManager.
static struct item item_sign = {
	.name = "Sign",
	.has_damage = false,
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.onItemPlace = item_sign_onItemPlace,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 10,
			.texture_y = 2,
		},
	},
};

extern struct item item_door_wood;

extern struct item item_door_iron;

extern struct item item_bucket;

extern struct item item_bucket_water;

extern struct item item_bucket_lava;

extern struct item item_bucket_milk;

static struct item item_slime_ball = {
	.name = "Slimeball",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 14,
			.texture_y = 1,
		},
	},
};

static struct item item_egg = {
	.name = "Egg",
	.has_damage = false,
	.max_stack = 16,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 12,
			.texture_y = 0,
		},
	},
};

// Beta ItemFishingRod(90) [740c583]: setMaxDamage(64), setMaxStackSize(1).
// No onItemPlace/onUseOnBlock -- like the bow/egg/snowball, its entire
// right-click behavior is the ItemFishingRod.onItemRightClick hook, which
// this engine dispatches by explicit item id (item_has_air_use +
// server_local.c's SRPC_USE_ITEM / SRPC_BLOCK_PLACE fall-through), not
// through the item vtable.
static struct item item_fishing_rod = {
	.name = "Fishing Rod",
	.has_damage = true,
	.max_damage = 64,
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY, // not a mining/melee tool -- wears only via
	                            // catchFish()'s variable return value, not
	                            // item_wear_on_block_break/hit_entity
	.render_data = {
		.item = {
			.texture_x = 5, // Beta setIconCoord(5, 4)
			.texture_y = 4,
		},
	},
};

static struct item item_bone = {
	.name = "Bone",
	.has_damage = false,
	.max_stack = 64,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 12,
			.texture_y = 1,
		},
	},
};

// Beta 1.7.3 music records (Record "13" + "Cat"), ids 2256/2257. Obtainable
// the Beta way: a creeper whose KILLING BLOW was a skeleton-fired arrow drops
// one (mob_die_tick). Icons: MCP b1.7.3 iconIndex 240/241 = items.png row 15,
// cols 0/1.
//
// jukebox-slice: onUseOnBlock wires ItemRecord.onItemUse (insert into an
// empty jukebox; item_record.c). Playback (2026-08-31) is wired through the
// S3 streaming machinery -- client_interface.c's CRPC_RECORD_EVENT handler
// calls audio_music_play_record_for_item/_stop_record (audio_music.c/.h) --
// still NO Mojang IP is ever bundled: a client with no imported
// <pack>/records/13.ogg + cat.ogg (tools/make_beta_pack.ps1's records
// stage) plays nothing, exactly like S3 background music with no imported
// <pack>/music/. See docs/briefs/NOTEBLOCK_JUKEBOX_BRIEF.md (the SIM design;
// predates the S3 wiring).
extern bool item_record_onUseOnBlock(struct server_local*, struct server_player* actor, struct item_data*,
									 struct block_info*, enum side);

static struct item item_record_13 = {
	.name = "13", // Beta names records by track title
	.has_damage = false,
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.onUseOnBlock = item_record_onUseOnBlock,
	.render_data = {
		.item = {
			.texture_x = 0,
			.texture_y = 15,
		},
	},
};

static struct item item_record_cat = {
	.name = "Cat",
	.has_damage = false,
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.onUseOnBlock = item_record_onUseOnBlock,
	.render_data = {
		.item = {
			.texture_x = 1,
			.texture_y = 15,
		},
	},
};

static struct item item_saddle = {
	.name = "Saddle",
	.has_damage = false,
	.max_stack = 1,
	.fuel = 0,
	.renderItem = render_item_flat,
	.armor.is_armor = false,
	.tool.type = TOOL_TYPE_ANY,
	.render_data = {
		.item = {
			.texture_x = 8,
			.texture_y = 6,
		},
	},
};

extern struct item item_sugarcane;
extern struct item item_egg_zombie;
extern struct item item_minecart;
extern struct item item_boat;
extern struct item item_minecart_chest;
extern struct item item_minecart_furnace;
