/* Goldens: [740c583: EntityWolf.writeEntityToNBT/readEntityFromNBT]. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "network/entity_archive.h"
#include "entity/entity_wolf.h"

int main(void) {
	struct archived_entity in = {0};
	in.kind = ARCHIVED_ENTITY_MONSTER;
	in.source_id = 44;
	in.pos[0] = 1.5F; in.pos[1] = 70.0F; in.pos[2] = 2.5F;
	in.health = 17;
	in.data.monster.monster_id = MONSTER_WOLF;
	in.data.monster.wolf_angry = true;
	in.data.monster.wolf_sitting = true;
	strcpy(in.data.monster.wolf_owner, "Local Player");
	in.data.monster.fuse = -1;
	in.data.monster.air = 300;
	in.data.monster.squid_phase_vel = 0.15F;
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
	const struct archived_monster_state* m = &parsed.entities[0].data.monster;
	assert(m->monster_id == MONSTER_WOLF && m->wolf_angry && m->wolf_sitting);
	assert(strcmp(m->wolf_owner, "Local Player") == 0);
	uint8_t flags = wolf_restored_flags(m->wolf_angry, m->wolf_sitting,
		m->wolf_owner);
	assert(wolf_flag_get(flags, WOLF_FLAG_TAMED));
	assert(!wolf_can_despawn(flags));
	assert(wolf_can_despawn(wolf_restored_flags(false, false, "")));
	chunk_entities_free(&parsed);
	chunk_entities_build_free(arena);
	puts("test_wolf_persistence: PASS");
	return 0;
}
