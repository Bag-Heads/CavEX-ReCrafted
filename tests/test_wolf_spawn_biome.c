/* Goldens: [740c583: BiomeGenForest/BiomeGenTaiga]. */
#include <assert.h>
#include <stdio.h>

#include "entity/monsters_object.h"
#include "worldgen/biome.h"

int main(void) {
	int base = animal_spawn_total_weight();
	assert(base == 40); /* sheep12/pig10/chicken10/cow8 */
	assert(animal_spawn_total_weight_biome(BIOME_FOREST) == 42);
	assert(animal_spawn_total_weight_biome(BIOME_TAIGA) == 42);
	assert(animal_spawn_total_weight_biome(BIOME_PLAINS) == base);
	for(int roll = 0; roll < base; roll++) {
		int legacy = animal_spawn_pick(roll);
		assert(animal_spawn_pick_biome(BIOME_PLAINS, roll) == legacy);
		assert(animal_spawn_pick_biome(BIOME_DESERT, roll) == legacy);
		assert(animal_spawn_pick_biome(BIOME_FOREST, roll) == legacy);
		assert(animal_spawn_pick_biome(BIOME_TAIGA, roll) == legacy);
	}
	assert(animal_spawn_pick_biome(BIOME_FOREST, 40) == MONSTER_WOLF);
	assert(animal_spawn_pick_biome(BIOME_FOREST, 41) == MONSTER_WOLF);
	assert(animal_spawn_pick_biome(BIOME_TAIGA, 40) == MONSTER_WOLF);
	assert(animal_spawn_pick_biome(BIOME_TAIGA, 41) == MONSTER_WOLF);
	assert(monster_max_spawned_in_chunk(MONSTER_WOLF) == 8);
	assert(monster_max_spawned_in_chunk(MONSTER_PIG) == 4);
	puts("test_wolf_spawn_biome: PASS");
	return 0;
}
