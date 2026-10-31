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

#include <stdarg.h>

#include "recipe.h"

array_recipe_t recipes_crafting;
array_recipe_shapeless_t recipes_crafting_shapeless;

static void add_tools() {
	// wood
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_WOOD_SWORD, .durability = 0, .count = 1},
		1, 3, (uint8_t[]) {1, 1, 2}, (struct item_data) {.id = BLOCK_PLANKS},
		false, (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_WOOD_PICKAXE, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 0, 2, 0, 0, 2, 0},
			   (struct item_data) {.id = BLOCK_PLANKS}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_WOOD_SHOVEL, .durability = 0, .count = 1},
			   1, 3, (uint8_t[]) {1, 2, 2},
			   (struct item_data) {.id = BLOCK_PLANKS}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_WOOD_AXE, .durability = 0, .count = 1},
		2, 3, (uint8_t[]) {1, 1, 1, 2, 0, 2},
		(struct item_data) {.id = BLOCK_PLANKS}, false,
		(struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_WOOD_HOE, .durability = 0, .count = 1},
		2, 3, (uint8_t[]) {1, 1, 0, 2, 0, 2},
		(struct item_data) {.id = BLOCK_PLANKS}, false,
		(struct item_data) {.id = ITEM_STICK}, false);

	// stone
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_STONE_SWORD, .durability = 0, .count = 1},
			   1, 3, (uint8_t[]) {1, 1, 2},
			   (struct item_data) {.id = BLOCK_COBBLESTONE}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_STONE_PICKAXE, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 0, 2, 0, 0, 2, 0},
			   (struct item_data) {.id = BLOCK_COBBLESTONE}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_STONE_SHOVEL, .durability = 0, .count = 1},
			   1, 3, (uint8_t[]) {1, 2, 2},
			   (struct item_data) {.id = BLOCK_COBBLESTONE}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_STONE_AXE, .durability = 0, .count = 1},
		2, 3, (uint8_t[]) {1, 1, 1, 2, 0, 2},
		(struct item_data) {.id = BLOCK_COBBLESTONE}, false,
		(struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_STONE_HOE, .durability = 0, .count = 1},
		2, 3, (uint8_t[]) {1, 1, 0, 2, 0, 2},
		(struct item_data) {.id = BLOCK_COBBLESTONE}, false,
		(struct item_data) {.id = ITEM_STICK}, false);

	// gold
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_GOLD_SWORD, .durability = 0, .count = 1},
		1, 3, (uint8_t[]) {1, 1, 2}, (struct item_data) {.id = ITEM_GOLD},
		false, (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_GOLD_PICKAXE, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 0, 2, 0, 0, 2, 0},
			   (struct item_data) {.id = ITEM_GOLD}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_GOLD_SHOVEL, .durability = 0, .count = 1},
			   1, 3, (uint8_t[]) {1, 2, 2},
			   (struct item_data) {.id = ITEM_GOLD}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_GOLD_AXE, .durability = 0, .count = 1},
		2, 3, (uint8_t[]) {1, 1, 1, 2, 0, 2},
		(struct item_data) {.id = ITEM_GOLD}, false,
		(struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_GOLD_HOE, .durability = 0, .count = 1},
		2, 3, (uint8_t[]) {1, 1, 0, 2, 0, 2},
		(struct item_data) {.id = ITEM_GOLD}, false,
		(struct item_data) {.id = ITEM_STICK}, false);

	// iron
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_IRON_SWORD, .durability = 0, .count = 1},
		1, 3, (uint8_t[]) {1, 1, 2}, (struct item_data) {.id = ITEM_IRON},
		false, (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_IRON_PICKAXE, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 0, 2, 0, 0, 2, 0},
			   (struct item_data) {.id = ITEM_IRON}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_IRON_SHOVEL, .durability = 0, .count = 1},
			   1, 3, (uint8_t[]) {1, 2, 2},
			   (struct item_data) {.id = ITEM_IRON}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_IRON_AXE, .durability = 0, .count = 1},
		2, 3, (uint8_t[]) {1, 1, 1, 2, 0, 2},
		(struct item_data) {.id = ITEM_IRON}, false,
		(struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_IRON_HOE, .durability = 0, .count = 1},
		2, 3, (uint8_t[]) {1, 1, 0, 2, 0, 2},
		(struct item_data) {.id = ITEM_IRON}, false,
		(struct item_data) {.id = ITEM_STICK}, false);

	// diamond
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_DIAMOND_SWORD, .durability = 0, .count = 1},
			   1, 3, (uint8_t[]) {1, 1, 2},
			   (struct item_data) {.id = ITEM_DIAMOND}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_DIAMOND_PICKAXE, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 0, 2, 0, 0, 2, 0},
			   (struct item_data) {.id = ITEM_DIAMOND}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_DIAMOND_SHOVEL, .durability = 0, .count = 1},
			   1, 3, (uint8_t[]) {1, 2, 2},
			   (struct item_data) {.id = ITEM_DIAMOND}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_DIAMOND_AXE, .durability = 0, .count = 1},
			   2, 3, (uint8_t[]) {1, 1, 1, 2, 0, 2},
			   (struct item_data) {.id = ITEM_DIAMOND}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_DIAMOND_HOE, .durability = 0, .count = 1},
			   2, 3, (uint8_t[]) {1, 1, 0, 2, 0, 2},
			   (struct item_data) {.id = ITEM_DIAMOND}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
}

static void add_armor() {
	// leather
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_LEATHER_HELMET, .durability = 0, .count = 1},
			   3, 2, (uint8_t[]) {1, 1, 1, 1, 0, 1},
			   (struct item_data) {.id = ITEM_LEATHER}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_LEATHER_CHESTPLATE, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 0, 1, 1, 1, 1, 1, 1, 1},
			   (struct item_data) {.id = ITEM_LEATHER}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_LEATHER_LEGGINGS, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 1, 0, 1, 1, 0, 1},
			   (struct item_data) {.id = ITEM_LEATHER}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_LEATHER_BOOTS, .durability = 0, .count = 1},
			   3, 2, (uint8_t[]) {1, 0, 1, 1, 0, 1},
			   (struct item_data) {.id = ITEM_LEATHER}, false);

	// chain
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_CHAIN_HELMET, .durability = 0, .count = 1},
			   3, 2, (uint8_t[]) {1, 1, 1, 1, 0, 1},
			   (struct item_data) {.id = BLOCK_FIRE}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_CHAIN_CHESTPLATE, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 0, 1, 1, 1, 1, 1, 1, 1},
			   (struct item_data) {.id = BLOCK_FIRE}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_CHAIN_LEGGINGS, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 1, 0, 1, 1, 0, 1},
			   (struct item_data) {.id = BLOCK_FIRE}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_CHAIN_BOOTS, .durability = 0, .count = 1},
			   3, 2, (uint8_t[]) {1, 0, 1, 1, 0, 1},
			   (struct item_data) {.id = BLOCK_FIRE}, false);

	// iron
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_IRON_HELMET, .durability = 0, .count = 1},
			   3, 2, (uint8_t[]) {1, 1, 1, 1, 0, 1},
			   (struct item_data) {.id = ITEM_IRON}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_IRON_CHESTPLATE, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 0, 1, 1, 1, 1, 1, 1, 1},
			   (struct item_data) {.id = ITEM_IRON}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_IRON_LEGGINGS, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 1, 0, 1, 1, 0, 1},
			   (struct item_data) {.id = ITEM_IRON}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_IRON_BOOTS, .durability = 0, .count = 1},
		3, 2, (uint8_t[]) {1, 0, 1, 1, 0, 1},
		(struct item_data) {.id = ITEM_IRON}, false);

	// gold
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_GOLD_HELMET, .durability = 0, .count = 1},
			   3, 2, (uint8_t[]) {1, 1, 1, 1, 0, 1},
			   (struct item_data) {.id = ITEM_GOLD}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_GOLD_CHESTPLATE, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 0, 1, 1, 1, 1, 1, 1, 1},
			   (struct item_data) {.id = ITEM_GOLD}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_GOLD_LEGGINGS, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 1, 0, 1, 1, 0, 1},
			   (struct item_data) {.id = ITEM_GOLD}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_GOLD_BOOTS, .durability = 0, .count = 1},
		3, 2, (uint8_t[]) {1, 0, 1, 1, 0, 1},
		(struct item_data) {.id = ITEM_GOLD}, false);

	// diamond
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_DIAMOND_HELMET, .durability = 0, .count = 1},
			   3, 2, (uint8_t[]) {1, 1, 1, 1, 0, 1},
			   (struct item_data) {.id = ITEM_DIAMOND}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_DIAMOND_CHESTPLATE, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 0, 1, 1, 1, 1, 1, 1, 1},
			   (struct item_data) {.id = ITEM_DIAMOND}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_DIAMOND_LEGGINGS, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 1, 0, 1, 1, 0, 1},
			   (struct item_data) {.id = ITEM_DIAMOND}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_DIAMOND_BOOTS, .durability = 0, .count = 1},
			   3, 2, (uint8_t[]) {1, 0, 1, 1, 0, 1},
			   (struct item_data) {.id = ITEM_DIAMOND}, false);
}

// FH-09: RecipesDyes.addRecipes, transcribed verbatim in Beta's own
// registration order. [740c583 RecipesDyes.java, BlockCloth.java]
static void add_dyes() {
	// The 16-iteration wool-recolour loop: dye(meta N) + white wool (meta 0)
	// -> 1 wool of meta BlockCloth.getDyeFromBlock(N). getDyeFromBlock and its
	// inverse getBlockFromDye are both `~x & 15` (self-inverse, 15-x) -- the
	// SAME formula already used for sheep dyeing in server_local.c
	// (`dye_color = (~held->durability) & 15`), so this is precedent, not a
	// new convention.
	for(uint8_t n = 0; n < 16; n++) {
		recipe_shapeless_add(
			recipes_crafting_shapeless,
			(struct item_data) {
				.id = BLOCK_WOOL, .durability = (uint16_t)(~n & 15), .count = 1},
			2, (struct item_data) {.id = ITEM_DYE, .durability = n}, true,
			(struct item_data) {.id = BLOCK_WOOL, .durability = 0}, true);
	}

	// The 15 explicit dye-mix/dye-source recipes that follow the loop.
	recipe_shapeless_add(recipes_crafting_shapeless,
						 (struct item_data) {
							 .id = ITEM_DYE, .durability = 11, .count = 2},
						 1, (struct item_data) {.id = BLOCK_FLOWER}, true);
	recipe_shapeless_add(
		recipes_crafting_shapeless,
		(struct item_data) {.id = ITEM_DYE, .durability = 1, .count = 2}, 1,
		(struct item_data) {.id = BLOCK_ROSE}, true);
	recipe_shapeless_add(
		recipes_crafting_shapeless,
		(struct item_data) {.id = ITEM_DYE, .durability = 15, .count = 3}, 1,
		(struct item_data) {.id = ITEM_BONE}, true);
	recipe_shapeless_add(
		recipes_crafting_shapeless,
		(struct item_data) {.id = ITEM_DYE, .durability = 9, .count = 2}, 2,
		(struct item_data) {.id = ITEM_DYE, .durability = 1}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 15}, true);
	recipe_shapeless_add(
		recipes_crafting_shapeless,
		(struct item_data) {.id = ITEM_DYE, .durability = 14, .count = 2}, 2,
		(struct item_data) {.id = ITEM_DYE, .durability = 1}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 11}, true);
	recipe_shapeless_add(
		recipes_crafting_shapeless,
		(struct item_data) {.id = ITEM_DYE, .durability = 10, .count = 2}, 2,
		(struct item_data) {.id = ITEM_DYE, .durability = 2}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 15}, true);
	recipe_shapeless_add(
		recipes_crafting_shapeless,
		(struct item_data) {.id = ITEM_DYE, .durability = 8, .count = 2}, 2,
		(struct item_data) {.id = ITEM_DYE, .durability = 0}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 15}, true);
	recipe_shapeless_add(
		recipes_crafting_shapeless,
		(struct item_data) {.id = ITEM_DYE, .durability = 7, .count = 2}, 2,
		(struct item_data) {.id = ITEM_DYE, .durability = 8}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 15}, true);
	recipe_shapeless_add(
		recipes_crafting_shapeless,
		(struct item_data) {.id = ITEM_DYE, .durability = 7, .count = 3}, 3,
		(struct item_data) {.id = ITEM_DYE, .durability = 0}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 15}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 15}, true);
	recipe_shapeless_add(
		recipes_crafting_shapeless,
		(struct item_data) {.id = ITEM_DYE, .durability = 12, .count = 2}, 2,
		(struct item_data) {.id = ITEM_DYE, .durability = 4}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 15}, true);
	recipe_shapeless_add(
		recipes_crafting_shapeless,
		(struct item_data) {.id = ITEM_DYE, .durability = 6, .count = 2}, 2,
		(struct item_data) {.id = ITEM_DYE, .durability = 4}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 2}, true);
	recipe_shapeless_add(
		recipes_crafting_shapeless,
		(struct item_data) {.id = ITEM_DYE, .durability = 5, .count = 2}, 2,
		(struct item_data) {.id = ITEM_DYE, .durability = 4}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 1}, true);
	recipe_shapeless_add(
		recipes_crafting_shapeless,
		(struct item_data) {.id = ITEM_DYE, .durability = 13, .count = 2}, 2,
		(struct item_data) {.id = ITEM_DYE, .durability = 5}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 9}, true);
	recipe_shapeless_add(
		recipes_crafting_shapeless,
		(struct item_data) {.id = ITEM_DYE, .durability = 13, .count = 3}, 3,
		(struct item_data) {.id = ITEM_DYE, .durability = 4}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 1}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 9}, true);
	recipe_shapeless_add(
		recipes_crafting_shapeless,
		(struct item_data) {.id = ITEM_DYE, .durability = 13, .count = 4}, 4,
		(struct item_data) {.id = ITEM_DYE, .durability = 4}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 1}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 1}, true,
		(struct item_data) {.id = ITEM_DYE, .durability = 15}, true);
}

void recipe_init() {
	array_recipe_init(recipes_crafting);
	array_recipe_shapeless_init(recipes_crafting_shapeless);
	add_dyes();

	recipe_add(recipes_crafting,
			   (struct item_data) {.id = ITEM_BOW, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {0, 1, 2, 1, 0, 2, 0, 1, 2},
			   (struct item_data) {.id = ITEM_STICK}, false,
			   (struct item_data) {.id = ITEM_STRING}, false);

	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_GOLD_CAST, .durability = 0, .count = 1},
		3, 3, (uint8_t[]) {1, 1, 1, 1, 1, 1, 1, 1, 1},
		(struct item_data) {.id = ITEM_GOLD}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_IRON_CAST, .durability = 0, .count = 1},
		3, 3, (uint8_t[]) {1, 1, 1, 1, 1, 1, 1, 1, 1},
		(struct item_data) {.id = ITEM_IRON}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = BLOCK_DIAMOND_CAST, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 1, 1, 1, 1, 1, 1},
			   (struct item_data) {.id = ITEM_DIAMOND}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = BLOCK_LAPIS_CAST, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 1, 1, 1, 1, 1, 1},
			   (struct item_data) {.id = ITEM_DYE, .durability = 4}, true);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_CHEST, .durability = 0, .count = 1}, 3,
		3, (uint8_t[]) {1, 1, 1, 1, 0, 1, 1, 1, 1},
		(struct item_data) {.id = BLOCK_PLANKS}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_IRON_CHEST, .durability = 0, .count = 1}, 3,
		3, (uint8_t[]) {1, 1, 1, 1, 0, 1, 1, 1, 1},
		(struct item_data) {.id = ITEM_IRON}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_FURNACE, .durability = 0, .count = 1},
		3, 3, (uint8_t[]) {1, 1, 1, 1, 0, 1, 1, 1, 1},
		(struct item_data) {.id = BLOCK_COBBLESTONE}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_JUKEBOX, .durability = 0, .count = 1},
		3, 3, (uint8_t[]) {1, 1, 1, 1, 2, 1, 1, 1, 1},
		(struct item_data) {.id = BLOCK_PLANKS}, false,
		(struct item_data) {.id = ITEM_DIAMOND}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_NOTEBLOCK, .durability = 0, .count = 1},
		3, 3, (uint8_t[]) {1, 1, 1, 1, 2, 1, 1, 1, 1},
		(struct item_data) {.id = BLOCK_PLANKS}, false,
		(struct item_data) {.id = ITEM_REDSTONE}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_BOOKSHELF, .durability = 0, .count = 1},
		3, 3, (uint8_t[]) {1, 1, 1, 2, 2, 2, 1, 1, 1},
		(struct item_data) {.id = BLOCK_PLANKS}, false,
		(struct item_data) {.id = ITEM_BOOK}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_TNT, .durability = 0, .count = 1}, 3, 3,
		(uint8_t[]) {1, 2, 1, 2, 1, 2, 1, 2, 1},
		(struct item_data) {.id = ITEM_GUNPOWDER}, false,
		(struct item_data) {.id = BLOCK_SAND}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_LADDER, .durability = 0, .count = 2}, 3,
		3, (uint8_t[]) {1, 0, 1, 1, 1, 1, 1, 0, 1},
		(struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_MINECART, .durability = 0, .count = 1}, 3, 2,
		(uint8_t[]) {1, 0, 1, 1, 1, 1}, (struct item_data) {.id = ITEM_IRON},
		false);
	// Beta CraftingManager: minecartCrate (chest minecart) = "A"/"B", A = chest,
	// B = (empty) minecart. Width 1, height 2: chest on top, minecart below.
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_MINECART_CHEST, .durability = 0, .count = 1},
			   1, 2, (uint8_t[]) {1, 2},
			   (struct item_data) {.id = BLOCK_CHEST}, false,
			   (struct item_data) {.id = ITEM_MINECART}, false);
	// Beta CraftingManager: minecartPowered (furnace minecart) = "A"/"B",
	// A = stoneOvenIdle (furnace), B = (empty) minecart.
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_MINECART_FURNACE, .durability = 0, .count = 1},
			   1, 2, (uint8_t[]) {1, 2},
			   (struct item_data) {.id = BLOCK_FURNACE}, false,
			   (struct item_data) {.id = ITEM_MINECART}, false);
	// Beta CraftingManager: boat = "# #"/"###", # = planks. 5 planks, U-shape.
	recipe_add(recipes_crafting,
			   (struct item_data) {.id = ITEM_BOAT, .durability = 0, .count = 1},
			   3, 2, (uint8_t[]) {1, 0, 1, 1, 1, 1},
			   (struct item_data) {.id = BLOCK_PLANKS}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_BUCKET, .durability = 0, .count = 1}, 3, 2,
		(uint8_t[]) {1, 0, 1, 0, 1, 0}, (struct item_data) {.id = ITEM_IRON},
		false);
	// Beta CraftingManager: cake = "AAA","BEB","CCC" with A = milk bucket,
	// B = sugar, E = egg, C = wheat. The 3 milk buckets come back as 3 EMPTY
	// buckets IN THE GRID via containerItem (SlotCrafting semantics -- see the
	// crafting consume loops). [740c583: CraftingManager.java:45]
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_CAKE, .durability = 0, .count = 1}, 3, 3,
		(uint8_t[]) {1, 1, 1, 2, 3, 2, 4, 4, 4},
		(struct item_data) {.id = ITEM_MILK_BUCKET}, false,
		(struct item_data) {.id = ITEM_SUGAR}, false,
		(struct item_data) {.id = ITEM_EGG}, false,
		(struct item_data) {.id = ITEM_WHEAT}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_RAIL, .durability = 0, .count = 16}, 3,
		3, (uint8_t[]) {1, 0, 1, 1, 2, 1, 1, 0, 1},
		(struct item_data) {.id = ITEM_IRON}, false,
		(struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = BLOCK_POWERED_RAIL, .durability = 0, .count = 6},
			   3, 3, (uint8_t[]) {1, 0, 1, 1, 2, 1, 1, 3, 1},
			   (struct item_data) {.id = ITEM_GOLD}, false,
			   (struct item_data) {.id = ITEM_STICK}, false,
			   (struct item_data) {.id = ITEM_REDSTONE}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = BLOCK_DETECTOR_RAIL, .durability = 0, .count = 6},
			   3, 3, (uint8_t[]) {1, 0, 1, 1, 2, 1, 1, 3, 1},
			   (struct item_data) {.id = ITEM_IRON}, false,
			   (struct item_data) {.id = BLOCK_STONE_PRESSURE_PLATE}, false,
			   (struct item_data) {.id = ITEM_REDSTONE}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = BLOCK_WOODEN_STAIRS, .durability = 0, .count = 4},
			   3, 3, (uint8_t[]) {1, 0, 0, 1, 1, 0, 1, 1, 1},
			   (struct item_data) {.id = BLOCK_PLANKS}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = BLOCK_STONE_STAIRS, .durability = 0, .count = 4},
			   3, 3, (uint8_t[]) {1, 0, 0, 1, 1, 0, 1, 1, 1},
			   (struct item_data) {.id = BLOCK_COBBLESTONE}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_APPLE_GOLDEN, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 1, 2, 1, 1, 1, 1},
			   (struct item_data) {.id = BLOCK_GOLD_CAST}, false,
			   (struct item_data) {.id = ITEM_APPLE}, false);
	// Beta CraftingManager:71-72 [740c583], in Beta's own registration order:
	//   pocketSundial (clock) = " # "/"#X#"/" # "  (# gold ingot, X redstone)
	//   compass               = " # "/"#X#"/" # "  (# iron ingot, X redstone)
	recipe_add(recipes_crafting,
			   (struct item_data) {.id = ITEM_CLOCK, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {0, 1, 0, 1, 2, 1, 0, 1, 0},
			   (struct item_data) {.id = ITEM_GOLD}, false,
			   (struct item_data) {.id = ITEM_REDSTONE}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {.id = ITEM_COMPASS, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {0, 1, 0, 1, 2, 1, 0, 1, 0},
			   (struct item_data) {.id = ITEM_IRON}, false,
			   (struct item_data) {.id = ITEM_REDSTONE}, false);
	// Beta CraftingManager:73 [740c583], immediately following compass in
	// Beta's own registration order: one map from a 3x3 ring of
	// paper around a compass -- 8
	// paper ring, compass center. Map part 2a (Batch F).
	recipe_add(recipes_crafting,
			   (struct item_data) {.id = ITEM_MAP, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 1, 2, 1, 1, 1, 1},
			   (struct item_data) {.id = ITEM_PAPER}, false,
			   (struct item_data) {.id = ITEM_COMPASS}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_DISPENSER, .durability = 0, .count = 1},
		3, 3, (uint8_t[]) {1, 1, 1, 1, 2, 1, 1, 3, 1},
		(struct item_data) {.id = BLOCK_COBBLESTONE}, false,
		(struct item_data) {.id = ITEM_BOW}, false,
		(struct item_data) {.id = ITEM_REDSTONE}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_FENCE, .durability = 0, .count = 2}, 3,
		2, (uint8_t[]) {1, 1, 1, 1, 1, 1},
		(struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_DOOR_WOOD, .durability = 0, .count = 1},
		2, 3, (uint8_t[]) {1, 1, 1, 1, 1, 1},
		(struct item_data) {.id = BLOCK_PLANKS}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_TRAP_DOOR, .durability = 0, .count = 2},
		3, 2, (uint8_t[]) {1, 1, 1, 1, 1, 1},
		(struct item_data) {.id = BLOCK_PLANKS}, false);
	// CR-03: Beta CraftingManager [740c583, CraftingManager.java:44] --
	//   registers one sign item: two rows of planks over a centred stick
	//  (planks row, planks row, stick in the middle column)
	// -- ONE sign ITEM (id 323 / shiftedIndex 67), not three of the signPost
	// BLOCK (BLOCK_SIGN/68). The shape below already matched Beta's "###"/
	// "###"/" X "; only the output id/count were wrong. The crafted item is
	// later placed via ItemSign.onItemUse into signPost/signWall -- that
	// placement mechanic is out of this slice's scope (registration +
	// safe-stacking only, see items.c/items_object.h).
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_SIGN, .durability = 0, .count = 1},
		3, 3, (uint8_t[]) {1, 1, 1, 1, 1, 1, 0, 2, 0},
		(struct item_data) {.id = BLOCK_PLANKS}, false,
		(struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_DOOR_IRON, .durability = 0, .count = 1},
		2, 3, (uint8_t[]) {1, 1, 1, 1, 1, 1},
		(struct item_data) {.id = ITEM_IRON}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_BOWL, .durability = 0, .count = 4}, 3, 2,
		(uint8_t[]) {1, 0, 1, 0, 1, 0}, (struct item_data) {.id = BLOCK_PLANKS},
		false);
	recipe_add(recipes_crafting,
			   (struct item_data) {.id = ITEM_BED, .durability = 0, .count = 1},
			   3, 2, (uint8_t[]) {1, 1, 1, 2, 2, 2},
			   (struct item_data) {.id = BLOCK_WOOL}, false,
			   (struct item_data) {.id = BLOCK_PLANKS}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_SHEARS, .durability = 0, .count = 1}, 2,
		2, (uint8_t[]) {0, 1, 1, 0}, (struct item_data) {.id = ITEM_IRON},
		false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_WORKBENCH, .durability = 0, .count = 1},
		2, 2, (uint8_t[]) {1, 1, 1, 1}, (struct item_data) {.id = BLOCK_PLANKS},
		false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_SANDSTONE, .durability = 0, .count = 1},
		2, 2, (uint8_t[]) {1, 1, 1, 1}, (struct item_data) {.id = BLOCK_SAND},
		false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = BLOCK_SNOW_BLOCK, .durability = 0, .count = 1},
			   2, 2, (uint8_t[]) {1, 1, 1, 1},
			   (struct item_data) {.id = ITEM_SNOW_BALL}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_CLAY, .durability = 0, .count = 1}, 2,
		2, (uint8_t[]) {1, 1, 1, 1}, (struct item_data) {.id = ITEM_CLAY_BALL},
		false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_BRICKS, .durability = 0, .count = 1}, 2,
		2, (uint8_t[]) {1, 1, 1, 1}, (struct item_data) {.id = ITEM_BRICK},
		false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_GLOWSTONE, .durability = 0, .count = 1},
		2, 2, (uint8_t[]) {1, 1, 1, 1},
		(struct item_data) {.id = ITEM_GLOWSTONE_DUST}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_WOOL, .durability = 0, .count = 1}, 2,
		2, (uint8_t[]) {1, 1, 1, 1}, (struct item_data) {.id = ITEM_STRING},
		false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_FLINT_STEEL, .durability = 0, .count = 1},
			   2, 2, (uint8_t[]) {1, 0, 0, 2},
			   (struct item_data) {.id = ITEM_IRON}, false,
			   (struct item_data) {.id = ITEM_FLINT}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_ARROW, .durability = 0, .count = 4}, 1,
		3, (uint8_t[]) {1, 2, 3}, (struct item_data) {.id = ITEM_FLINT}, false,
		(struct item_data) {.id = ITEM_STICK}, false,
		(struct item_data) {.id = ITEM_FEATHER}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_MUSHROOM_STEW, .durability = 0, .count = 1},
			   1, 3, (uint8_t[]) {1, 2, 3},
			   (struct item_data) {.id = BLOCK_RED_MUSHROOM}, false,
			   (struct item_data) {.id = BLOCK_BROWM_MUSHROOM}, false,
			   (struct item_data) {.id = ITEM_BOWL}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_MUSHROOM_STEW, .durability = 0, .count = 1},
			   1, 3, (uint8_t[]) {1, 2, 3},
			   (struct item_data) {.id = BLOCK_BROWM_MUSHROOM}, false,
			   (struct item_data) {.id = BLOCK_RED_MUSHROOM}, false,
			   (struct item_data) {.id = ITEM_BOWL}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_PAPER, .durability = 0, .count = 3}, 3,
		1, (uint8_t[]) {1, 1, 1}, (struct item_data) {.id = ITEM_REED}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_BOOK, .durability = 0, .count = 1}, 1, 3,
		(uint8_t[]) {1, 1, 1}, (struct item_data) {.id = ITEM_PAPER}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_SLAB, .durability = 3, .count = 3}, 3,
		1, (uint8_t[]) {1, 1, 1}, (struct item_data) {.id = BLOCK_COBBLESTONE},
		false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_SLAB, .durability = 0, .count = 3}, 3,
		1, (uint8_t[]) {1, 1, 1}, (struct item_data) {.id = BLOCK_STONE},
		false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_SLAB, .durability = 1, .count = 3}, 3,
		1, (uint8_t[]) {1, 1, 1}, (struct item_data) {.id = BLOCK_SANDSTONE},
		false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_SLAB, .durability = 2, .count = 3}, 3,
		1, (uint8_t[]) {1, 1, 1}, (struct item_data) {.id = BLOCK_PLANKS},
		false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_BREAD, .durability = 0, .count = 1}, 3,
		1, (uint8_t[]) {1, 1, 1}, (struct item_data) {.id = ITEM_WHEAT}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_STICK, .durability = 0, .count = 4}, 1,
		2, (uint8_t[]) {1, 1}, (struct item_data) {.id = BLOCK_PLANKS}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_TORCH, .durability = 0, .count = 4}, 1,
		2, (uint8_t[]) {1, 2}, (struct item_data) {.id = ITEM_COAL}, false,
		(struct item_data) {.id = ITEM_STICK}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = BLOCK_PUMPKIN_LIT, .durability = 0, .count = 1},
			   1, 2, (uint8_t[]) {1, 2},
			   (struct item_data) {.id = BLOCK_PUMPKIN}, false,
			   (struct item_data) {.id = BLOCK_TORCH}, false);
	// Beta CraftingManager: painting = "###"/"#X#"/"###", # = stick (8),
	// X = cloth (any wool colour, match_durability=false -- same idiom as
	// the bed recipe above).
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_PAINTING, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 1, 2, 1, 1, 1, 1},
			   (struct item_data) {.id = ITEM_STICK}, false,
			   (struct item_data) {.id = BLOCK_WOOL}, false);
	// Beta CraftingManager: lever = "X"/"#", X = stick, # = cobblestone.
	// Width 1, height 2: stick on top, cobblestone below.
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = BLOCK_LEVER, .durability = 0, .count = 1},
			   1, 2, (uint8_t[]) {1, 2},
			   (struct item_data) {.id = ITEM_STICK}, false,
			   (struct item_data) {.id = BLOCK_COBBLESTONE}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = BLOCK_REDSTONE_TORCH_LIT, .durability = 0, .count = 1},
			   1, 2, (uint8_t[]) {1, 2},
			   (struct item_data) {.id = ITEM_REDSTONE}, false,
			   (struct item_data) {.id = ITEM_STICK}, false);
	// Beta CraftingManager: repeater = "#X#"/"III" (# lit redstone torch, X redstone dust, I stone).
	recipe_add(recipes_crafting,
			   (struct item_data) {
				   .id = ITEM_REDSTONE_REPEATER, .durability = 0, .count = 1},
			   3, 2, (uint8_t[]) {1, 2, 1, 3, 3, 3},
			   (struct item_data) {.id = BLOCK_REDSTONE_TORCH_LIT}, false,
			   (struct item_data) {.id = ITEM_REDSTONE}, false,
			   (struct item_data) {.id = BLOCK_STONE}, false);
	// Beta CraftingManager: pistonBase = "TTT"/"#X#"/"#R#" (T planks, # cobble, X iron ingot, R redstone).
	recipe_add(recipes_crafting,
			   (struct item_data) {.id = BLOCK_PISTON_BASE, .durability = 0, .count = 1},
			   3, 3, (uint8_t[]) {1, 1, 1, 2, 3, 2, 2, 4, 2},
			   (struct item_data) {.id = BLOCK_PLANKS}, false,
			   (struct item_data) {.id = BLOCK_COBBLESTONE}, false,
			   (struct item_data) {.id = ITEM_IRON}, false,
			   (struct item_data) {.id = ITEM_REDSTONE}, false);
	// Beta CraftingManager: pistonStickyBase = "S"/"P" (S slimeball, P piston). Slimeball exists (id 341).
	recipe_add(recipes_crafting,
			   (struct item_data) {.id = BLOCK_PISTON_STICKY, .durability = 0, .count = 1},
			   1, 2, (uint8_t[]) {1, 2},
			   (struct item_data) {.id = ITEM_SLIME_BALL}, false,
			   (struct item_data) {.id = BLOCK_PISTON_BASE}, false);
	recipe_add(recipes_crafting,
			   (struct item_data) {.id = BLOCK_STONE_PRESSURE_PLATE,
								   .durability = 0,
								   .count = 1},
			   2, 1, (uint8_t[]) {1, 1}, (struct item_data) {.id = BLOCK_STONE},
			   false);
	// Beta CraftingManager: button = 2 stone stacked vertically ("#" / "#").
	recipe_add(recipes_crafting,
			   (struct item_data) {.id = BLOCK_STONE_BUTTON,
								   .durability = 0,
								   .count = 1},
			   1, 2, (uint8_t[]) {1, 1}, (struct item_data) {.id = BLOCK_STONE},
			   false);
	recipe_add(recipes_crafting,
			   (struct item_data) {.id = BLOCK_WOOD_PRESSURE_PLATE,
								   .durability = 0,
								   .count = 1},
			   2, 1, (uint8_t[]) {1, 1},
			   (struct item_data) {.id = BLOCK_PLANKS}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_GOLD, .durability = 0, .count = 9}, 1, 1,
		(uint8_t[]) {1}, (struct item_data) {.id = BLOCK_GOLD_CAST}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_IRON, .durability = 0, .count = 9}, 1, 1,
		(uint8_t[]) {1}, (struct item_data) {.id = BLOCK_IRON_CAST}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_DIAMOND, .durability = 0, .count = 9}, 1,
		1, (uint8_t[]) {1}, (struct item_data) {.id = BLOCK_DIAMOND_CAST},
		false);
	recipe_add(recipes_crafting,
			   (struct item_data) {.id = ITEM_DYE, .durability = 4, .count = 9},
			   1, 1, (uint8_t[]) {1},
			   (struct item_data) {.id = BLOCK_LAPIS_CAST}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_SUGAR, .durability = 0, .count = 1}, 1,
		1, (uint8_t[]) {1}, (struct item_data) {.id = ITEM_REED}, false);
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = BLOCK_PLANKS, .durability = 0, .count = 4}, 1,
		1, (uint8_t[]) {1}, (struct item_data) {.id = BLOCK_LOG}, false);
	// FH-10: Beta RecipesFood -- addRecipe(cookie x8, "#X#", '#' wheat,
	// 'X' dyePowder meta 3 (cocoa)). The shaped engine already supports a
	// meta-sensitive ingredient via match_durability (see the BLOCK_LAPIS_CAST
	// / ITEM_DYE durability=4 recipe above) -- no engine change needed.
	// [740c583 RecipesFood.java]
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_COOKIE, .durability = 0, .count = 8},
		3, 1, (uint8_t[]) {1, 2, 1},
		(struct item_data) {.id = ITEM_WHEAT}, false,
		(struct item_data) {.id = ITEM_DYE, .durability = 3}, true);
	// FH-11: Beta CraftingManager -- addRecipe(fishingRod, "  #", " #X", "# X",
	// '#' stick, 'X' silk==string). Row-major shape: (2,0)=stick, (1,1)=stick,
	// (2,1)=string, (0,2)=stick, (2,2)=string.
	// [740c583 CraftingManager.java: fishingRod recipe]
	recipe_add(
		recipes_crafting,
		(struct item_data) {.id = ITEM_FISHING_ROD, .durability = 0, .count = 1},
		3, 3, (uint8_t[]) {0, 0, 1, 0, 1, 2, 1, 0, 2},
		(struct item_data) {.id = ITEM_STICK}, false,
		(struct item_data) {.id = ITEM_STRING}, false);

	add_tools();
	add_armor();
}

void recipe_add(array_recipe_t recipes, struct item_data result, size_t width,
				size_t height, uint8_t* shape, ...) {
	assert(recipes && width > 0 && height > 0 && width * height <= 9 && shape);

	size_t count = 0;
	for(size_t k = 0; k < width * height; k++) {
		if(shape[k] > count)
			count = shape[k];
	}

	assert(count > 0 && count <= 9);

	struct recipe_ingredients ingredients[9];

	va_list inputs;
	va_start(inputs, shape);

	for(size_t k = 0; k < count; k++) {
		ingredients[k].item = va_arg(inputs, struct item_data);
		ingredients[k].match_durability = (bool)va_arg(inputs, int);
	}

	va_end(inputs);

	struct recipe r = (struct recipe) {
		.result = result,
		.width = width,
		.height = height,
	};

	for(size_t k = 0; k < width * height; k++) {
		if(shape[k] > 0) {
			r.shape[k] = ingredients[shape[k] - 1];
		} else {
			r.shape[k].item.id = 0;
		}
	}

	array_recipe_push_back(recipes, r);
}

bool recipe_match(array_recipe_t recipes, struct item_data slots[9],
				  bool slot_empty[9], struct item_data* result) {
	assert(recipes && slots && slot_empty && result);

	array_recipe_it_t it;
	array_recipe_it(it, recipes);

	while(!array_recipe_end_p(it)) {
		struct recipe* current = array_recipe_ref(it);

		for(size_t y = 0; y <= 3 - current->height; y++) {
			for(size_t x = 0; x <= 3 - current->width; x++) {
				bool match_outside_empty = true;

				// check that outside of pattern is empty
				for(size_t py = 0; py < 3 && match_outside_empty; py++) {
					for(size_t px = 0; px < 3 && match_outside_empty; px++) {
						if((px < x || px >= x + current->width || py < y
							|| py >= y + current->height)
						   && !slot_empty[px + py * 3])
							match_outside_empty = false;
					}
				}

				// check pattern itself
				for(int flip = 0; flip <= 1; flip++) {
					bool match = match_outside_empty;

					for(size_t py = 0; py < current->height && match; py++) {
						for(size_t px = 0; px < current->width && match; px++) {
							size_t slots_idx = (px + x) + (py + y) * 3;
							size_t shape_idx
								= (flip ? (current->width - 1 - px) : px)
								+ py * current->width;

							if(current->shape[shape_idx].item.id == 0) {
								if(!slot_empty[slots_idx])
									match = false;
							} else {
								if(slot_empty[slots_idx]
								   || current->shape[shape_idx].item.id
									   != slots[slots_idx].id
								   || (current->shape[shape_idx].match_durability
									   && current->shape[shape_idx]
											   .item.durability
										   != slots[slots_idx].durability))
									match = false;
							}
						}
					}

					if(match) {
						*result = current->result;
						return true;
					}
				}
			}
		}

		array_recipe_next(it);
	}

	return false;
}

void recipe_shapeless_add(array_recipe_shapeless_t recipes,
						  struct item_data result, size_t count, ...) {
	assert(recipes && count > 0 && count <= 9);

	struct recipe_shapeless r = (struct recipe_shapeless) {
		.result = result,
		.count = count,
	};

	va_list inputs;
	va_start(inputs, count);

	for(size_t k = 0; k < count; k++) {
		r.ingredients[k].item = va_arg(inputs, struct item_data);
		r.ingredients[k].match_durability = (bool)va_arg(inputs, int);
	}

	va_end(inputs);

	array_recipe_shapeless_push_back(recipes, r);
}

// Beta ShapelessRecipes.matches: every non-empty grid slot must consume
// exactly one not-yet-used required ingredient (id match, and durability
// match when that ingredient's match_durability is set); once every slot is
// accounted for, every required ingredient must have been used (no leftovers
// in either direction). Works unmodified for both the 2x2 player grid and the
// 3x3 workbench grid -- it never looks at position, only at which of the 9
// slots are non-empty (mirrors Beta's InventoryCrafting.getStackInRowAndColumn
// scan over the full matches() loop). [740c583 ShapelessRecipes.java]
bool recipe_match_shapeless(array_recipe_shapeless_t recipes,
							struct item_data slots[9], bool slot_empty[9],
							struct item_data* result) {
	assert(recipes && slots && slot_empty && result);

	array_recipe_shapeless_it_t it;
	array_recipe_shapeless_it(it, recipes);

	while(!array_recipe_shapeless_end_p(it)) {
		struct recipe_shapeless* current = array_recipe_shapeless_ref(it);
		bool ingredient_used[9] = {false};
		bool ok = true;

		for(size_t s = 0; s < 9 && ok; s++) {
			if(slot_empty[s])
				continue;

			bool found = false;

			for(size_t k = 0; k < current->count; k++) {
				if(ingredient_used[k])
					continue;

				struct recipe_shapeless_ingredient* ing
					= &current->ingredients[k];

				if(ing->item.id == slots[s].id
				   && (!ing->match_durability
					   || ing->item.durability == slots[s].durability)) {
					ingredient_used[k] = true;
					found = true;
					break;
				}
			}

			if(!found)
				ok = false;
		}

		if(ok) {
			for(size_t k = 0; k < current->count; k++) {
				if(!ingredient_used[k]) {
					ok = false;
					break;
				}
			}
		}

		if(ok) {
			*result = current->result;
			return true;
		}

		array_recipe_shapeless_next(it);
	}

	return false;
}
