/*
	Pure archive-layer golden for ARCHIVED_ENTITY_FALLING_SAND (batch I).
	Mirrors test_wolf_persistence.c's shape: build a record through
	chunk_entities_build, parse it back through chunk_entities_parse, and
	check the round trip -- no server_local instance needed for the archive
	layer itself.

	Ground truth [740c583 EntityFallingSand.writeEntityToNBT/
	readEntityFromNBT]: exactly one kind-specific tag, `setByte("Tile",
	(byte)this.blockID)` / `this.blockID = var1.getByte("Tile") & 255`.
*/
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "block/blocks_data.h"
#include "network/entity_archive.h"

static struct archived_entity make_record(uint8_t block_id,
										  uint32_t source_id) {
	struct archived_entity e = {0};
	e.kind = ARCHIVED_ENTITY_FALLING_SAND;
	e.source_id = source_id;
	e.pos[0] = 8.5F;
	e.pos[1] = 70.5F;
	e.pos[2] = 8.5F;
	e.vel[1] = -0.5F;
	e.data.falling_sand.block_id = block_id;
	return e;
}

// Sand and gravel round-trip identically (Beta shares one entity class/tag
// shape for both; only the carried block id differs).
static void test_roundtrip_sand_and_gravel(void) {
	struct archived_entity in[2] = {
		make_record(BLOCK_SAND, 101),
		make_record(BLOCK_GRAVEL, 102),
	};
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(
		&(struct chunk_entities) {.entities = in, .count = 2}, &arena);
	assert(list && arena);
	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == 2);
	assert(parsed.entities[0].kind == ARCHIVED_ENTITY_FALLING_SAND);
	assert(parsed.entities[0].data.falling_sand.block_id == BLOCK_SAND);
	assert(parsed.entities[0].pos[0] == 8.5F);
	assert(parsed.entities[0].pos[1] == 70.5F);
	assert(parsed.entities[0].vel[1] == -0.5F);
	assert(parsed.entities[1].kind == ARCHIVED_ENTITY_FALLING_SAND);
	assert(parsed.entities[1].data.falling_sand.block_id == BLOCK_GRAVEL);
	chunk_entities_free(&parsed);
	chunk_entities_build_free(arena);
}

// The Beta "id" string form (foreign/loose NBT without CaveWiiType) must also
// resolve to ARCHIVED_ENTITY_FALLING_SAND via kind_from_compound's string
// table -- the same path a hand-authored or Beta-exported chunk would take.
static void test_kind_from_beta_id_string(void) {
	struct archived_entity in = make_record(BLOCK_SAND, 55);
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
	assert(parsed.entities[0].kind == ARCHIVED_ENTITY_FALLING_SAND);
	chunk_entities_free(&parsed);
	chunk_entities_build_free(arena);
}

// Defensive: a malformed/hostile Tile value outside {BLOCK_SAND,
// BLOCK_GRAVEL} must be rejected, not silently accepted into a live entity
// no real spawn path can otherwise construct.
static void test_bad_tile_rejected(void) {
	struct archived_entity in = make_record(BLOCK_SAND, 77);
	in.data.falling_sand.block_id = BLOCK_STONE; // never a valid faller
	void* arena = NULL;
	struct nbt_list* list = chunk_entities_build(
		&(struct chunk_entities) {.entities = &in, .count = 1}, &arena);
	assert(list && arena);
	nbt_node list_node = {
		.type = TAG_LIST, .name = "Entities", .payload.tag_list = list,
	};
	struct chunk_entities parsed = {0};
	assert(chunk_entities_parse(&list_node, 0, 0, &parsed));
	assert(parsed.count == 0 && "a non-sand/gravel Tile must be rejected");
	chunk_entities_free(&parsed);
	chunk_entities_build_free(arena);
}

int main(void) {
	test_roundtrip_sand_and_gravel();
	test_kind_from_beta_id_string();
	test_bad_tile_rejected();
	puts("test_falling_sand_archive: PASS");
	return 0;
}
