/*
	Golden test for the Giant (Beta 1.7.3 EntityGiantZombie, EntityList
	"Giant"/53) -- the maintainer's in-scope-low-pri parity item.

	Ground truth [decompile 740c583 EntityGiantZombie]: extends EntityMob
	DIRECTLY (not EntityZombie). Ctor: moveSpeed 0.5F; attackStrength 50;
	`health *= 10` on EntityLiving's 20 -> 200; `yOffset *= 6`;
	setSize(width*6, height*6) on the biped 0.6x1.8 -> 3.6 x 10.8 (eye =
	10.8 * 0.85 = 9.18). No getDropItemId override on its chain -> drops
	NOTHING (the feather is EntityZombie's own override). It appears in NO
	Beta spawn list -- never spawns naturally -- and EntityList maps it as
	"Giant" id 53, so a spawner TE naming "Giant" is its only in-world
	source.

	Pinned here:
	  1. the def-table goldens (health/box/eye/speed/flags/drop);
	  2. mob_melee_damage(GIANT) = 50 half-hearts * 8 = 400;
	  3. the NEVER-NATURAL invariant, exhaustively: every roll of every
	     spawn picker (overworld hostile, nether hostile, animal, and the
	     forest/taiga biome animal tables) maps to some OTHER mob -- the
	     mob-spawning analogue of the crafted-blocks-never-generate
	     worldgen invariant;
	  4. spawner_mob_from_id("Giant") resolves (the reachability path)
	     while the WORLDGEN dungeon pick surface (spawner_mob_from_meta /
	     spawner_mob_to_id) remains the byte-exact 4-entry Beta table;
	  5. the archive round-trip: a Giant saves under Beta's "Giant" id
	     string and restores to MONSTER_GIANT with its health intact
	     (chunk_entities_build -> chunk_entities_parse, the
	     test_falling_sand_archive.c pattern).
*/

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "entity/combat.h"
#include "entity/entity.h"
#include "entity/monsters_object.h"
#include "network/entity_archive.h"
#include "network/spawner_mob_id.h"

static void check_def_goldens(void) {
	const struct monster_def* d = monster_def_get(MONSTER_GIANT);
	assert(strcmp(d->name, "Giant") == 0);
	assert(d->health == 200 && "EntityLiving 20 * 10");
	assert(d->drop == 0 && d->drop_count == 0
		   && "no getDropItemId override on the EntityMob chain");
	assert(d->spawn_weight == 0 && d->animal_weight == 0
		   && d->nether_weight == 0 && "in NO Beta spawn list");
	assert(d->move_speed == 0.5f);
	assert(d->box_w == 0.6f * 6.0f && d->box_h == 1.8f * 6.0f
		   && "Beta float product of the 0.6x1.8 biped * 6");
	assert(d->eye_height == (1.8f * 6.0f) * 0.85f
		   && "Beta float product height * 0.85F");
	assert(!d->passive && !d->water_creature && !d->fall_immune
		   && !d->fire_immune && !d->no_light_gate);
	printf("  def goldens OK (hp 200, 3.6x10.8, eye 9.18, speed 0.5, "
		   "no drops, all spawn weights 0)\n");
}

static void check_melee_damage(void) {
	assert(mob_melee_damage(MONSTER_GIANT) == 400
		   && "attackStrength 50 -> 50 * 8 = 400 on the 160-pt player scale");
	printf("  melee damage OK (400 = 25 hearts, one-hit lethal as Beta)\n");
}

static void check_never_natural(void) {
	for(int r = 0; r < monster_spawn_total_weight(); r++)
		assert(monster_spawn_pick(r) != MONSTER_GIANT
			   && "overworld hostile table must never yield the Giant");
	for(int r = 0; r < monster_spawn_total_weight_dim(-1); r++)
		assert(monster_spawn_pick_dim(-1, r) != MONSTER_GIANT
			   && "nether table must never yield the Giant");
	for(int r = 0; r < animal_spawn_total_weight(); r++)
		assert(animal_spawn_pick(r) != MONSTER_GIANT
			   && "creature table must never yield the Giant");
	// Biome-extended creature tables (forest/taiga append the wolf row) --
	// sweep every biome id the picker accepts a weight for.
	for(int biome = 0; biome < 16; biome++)
		for(int r = 0; r < animal_spawn_total_weight_biome(biome); r++)
			assert(animal_spawn_pick_biome(biome, r) != MONSTER_GIANT
				   && "no biome creature table may yield the Giant");
	printf("  never-natural invariant OK (every roll of every picker)\n");
}

static void check_spawner_mapping(void) {
	assert(spawner_mob_from_id("Giant") == MONSTER_GIANT
		   && "a spawner TE naming Beta's \"Giant\" must be live");
	// The WORLDGEN dungeon pick surface stays the byte-exact Beta 4-table:
	// nextInt(4) -> Skeleton/Zombie/Zombie/Spider, no Giant anywhere.
	for(int m = 0; m < 4; m++) {
		assert(spawner_mob_from_meta((uint8_t)m) != MONSTER_GIANT);
		assert(strcmp(spawner_mob_to_id((uint8_t)m), "Giant") != 0);
	}
	assert(spawner_mob_to_id(4) == NULL); // table stays 4 entries exactly
	printf("  spawner mapping OK (\"Giant\" live; worldgen pick table "
		   "untouched)\n");
}

static void check_archive_roundtrip(void) {
	struct archived_entity in = {0};
	in.kind = ARCHIVED_ENTITY_MONSTER;
	in.source_id = 77;
	in.pos[0] = 8.5F;
	in.pos[1] = 70.0F;
	in.pos[2] = 8.5F;
	in.health = 200;
	in.data.monster.monster_id = MONSTER_GIANT;

	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(
		&(struct chunk_entities) {.entities = &in, .count = 1}, &arena);
	assert(list && arena);
	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == 1);
	assert(parsed.entities[0].kind == ARCHIVED_ENTITY_MONSTER);
	assert(parsed.entities[0].data.monster.monster_id == MONSTER_GIANT
		   && "the \"Giant\" id string must restore to MONSTER_GIANT");
	assert(parsed.entities[0].health == 200);
	chunk_entities_free(&parsed);
	chunk_entities_build_free(arena);
	printf("  archive round-trip OK (saves as Beta \"Giant\", restores id + "
		   "health)\n");
}

int main(void) {
	printf("test_giant: Beta EntityGiantZombie parity "
		   "(decompile 740c583)\n");
	check_def_goldens();
	check_melee_damage();
	check_never_natural();
	check_spawner_mapping();
	check_archive_roundtrip();
	printf("test_giant: PASS\n");
	return 0;
}
