/*
	FH-17 save-version-4 migration test: dungeon mob spawners move off a
	metadata-encoded mob type (2 bits smuggled into the spawner block's
	metadata nibble) to a faithful MobSpawner tile entity (Beta
	TileEntityMobSpawner's EntityId/Delay tags), per-instance provenance-safe
	-- the FH-07 (iron chest) migration test pattern
	(tests/test_iron_chest_migrate.c), adapted to region_archive_migrate_mob_
	spawner_tes's simpler, unconditional provenance rule (see its header in
	region_archive.h): a BLOCK_MOB_SPAWNER that already owns a MobSpawner TE
	is untouched regardless of world creation provenance; one with no TE is
	unambiguously this port's legacy encoding.

	Fixture chunk (single region r.0.0.mcr, chunk 0,0):
	  (2,64,2)  BLOCK_MOB_SPAWNER, metadata=1, WITH an existing MobSpawner TE
	            (EntityId="Skeleton", Delay=137) -> provenance-safe: untouched,
	            metadata included (proves the leave-alone rule covers the
	            block too, not just the TE).
	  (5,64,5)  BLOCK_MOB_SPAWNER, metadata=5 (masks to pick index 1 =
	            "Zombie"), NO TE -> synthesized: a new MobSpawner TE
	            (EntityId="Zombie", Delay=20) appears, and the metadata nibble
	            is zeroed to Beta's own faithful spawner-block value.
	  (8,64,8)  BLOCK_STONE, metadata=2 -> not a spawner block at all; proves
	            the pass never touches unrelated blocks.

	Proven, in order:
	  1) One pass: only the TE-less spawner is synthesized; the TE'd one (and
	     its metadata) and the unrelated stone block survive byte-for-byte.
	  2) Idempotency: re-running the full rewrite reports zero further work.
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0777)
#define RMDIR(p) rmdir(p)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "block/blocks_data.h"
#include "network/region_archive.h"
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"
#include "platform/mem2.h"

static char s_fixture_TMP_WORLD[128]; /* was #define TMP_WORLD='test_mob_spawner_migrate_tmp' (a literal) */
#define TMP_WORLD s_fixture_TMP_WORLD

#define COLUMN_BLOCKS ((size_t)CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT)

// Same index layout as region_archive.c / server_world.c:
// index = y + (z + x*CHUNK_SIZE) * WORLD_HEIGHT.
static size_t column_index(int x, int y, int z) {
	return (size_t)y + ((size_t)z + (size_t)x * CHUNK_SIZE) * WORLD_HEIGHT;
}

static void metadata_set(uint8_t* data, size_t index, uint8_t value) {
	unsigned shift = (unsigned)(index & 1) * 4;
	data[index / 2] = (uint8_t)((data[index / 2] & ~(0x0FU << shift))
		| ((value & 0x0F) << shift));
}
static uint8_t metadata_get(const uint8_t* data, size_t index) {
	return (data[index / 2] >> ((index & 1) * 4)) & 0x0F;
}

static void fixture_remove(void) {
	remove(test_fixture_join(TMP_WORLD, "/region/r.0.0.mcr"));
	RMDIR(test_fixture_join(TMP_WORLD, "/region"));
	RMDIR(TMP_WORLD);
}

static void fixture_create(void) {
	fixture_remove();
	assert(MKDIR(TMP_WORLD) == 0);
	assert(MKDIR(test_fixture_join(TMP_WORLD, "/region")) == 0);
	string_t world;
	string_init_set_str(world, TMP_WORLD);

	struct server_chunk chunk = {0};
	chunk.ids = calloc(COLUMN_BLOCKS, 1);
	chunk.metadata = calloc(COLUMN_BLOCKS / 2, 1);
	chunk.lighting_sky = calloc(COLUMN_BLOCKS / 2, 1);
	chunk.lighting_torch = calloc(COLUMN_BLOCKS / 2, 1);
	chunk.heightmap = calloc(CHUNK_SIZE * CHUNK_SIZE, 1);
	assert(chunk.ids && chunk.metadata && chunk.lighting_sky
		   && chunk.lighting_torch && chunk.heightmap);
	chunk.populated = true;

	chunk.ids[column_index(2, 64, 2)] = BLOCK_MOB_SPAWNER; // TE-proven
	metadata_set(chunk.metadata, column_index(2, 64, 2), 1);
	chunk.ids[column_index(5, 64, 5)] = BLOCK_MOB_SPAWNER; // bare (legacy)
	metadata_set(chunk.metadata, column_index(5, 64, 5), 5);
	chunk.ids[column_index(8, 64, 8)] = BLOCK_STONE; // unrelated block
	metadata_set(chunk.metadata, column_index(8, 64, 8), 2);

	struct te_spawner spawner;
	memset(&spawner, 0, sizeof(spawner));
	spawner.x = 2;
	spawner.y = 64;
	spawner.z = 2;
	strcpy(spawner.mob_id, "Skeleton");
	spawner.delay = 137;
	struct chunk_tile_entities te = {0};
	te.spawners = &spawner;
	te.n_spawner = 1;

	struct region_archive region;
	assert(region_archive_create_new(
		&region, world, 0, 0, WORLD_DIM_OVERWORLD));
	assert(region_archive_set_blocks(&region, 0, 0, &chunk, &te));
	assert(region_archive_sync(&region));
	assert(region_archive_destroy(&region));

	free(chunk.ids);
	free(chunk.metadata);
	free(chunk.lighting_sky);
	free(chunk.lighting_torch);
	free(chunk.heightmap);
	string_clear(world);
}

struct chunk_state {
	uint8_t id_proven, meta_proven;
	uint8_t id_bare, meta_bare;
	uint8_t id_stone, meta_stone;
	int n_spawner;
	bool proven_present;
	char proven_mob_id[TE_SPAWNER_MOB_ID_MAX];
	int16_t proven_delay;
	bool bare_present;
	char bare_mob_id[TE_SPAWNER_MOB_ID_MAX];
	int16_t bare_delay;
};

static void read_state(struct chunk_state* out) {
	string_t world;
	string_init_set_str(world, TMP_WORLD);
	struct region_archive region;
	assert(region_archive_create(&region, world, 0, 0, WORLD_DIM_OVERWORLD));
	struct server_chunk loaded = {0};
	struct chunk_tile_entities te = {0};
	assert(region_archive_get_blocks(&region, 0, 0, &loaded, &te));
	assert(region_archive_destroy(&region));

	memset(out, 0, sizeof(*out));
	out->id_proven = loaded.ids[column_index(2, 64, 2)];
	out->meta_proven = metadata_get(loaded.metadata, column_index(2, 64, 2));
	out->id_bare = loaded.ids[column_index(5, 64, 5)];
	out->meta_bare = metadata_get(loaded.metadata, column_index(5, 64, 5));
	out->id_stone = loaded.ids[column_index(8, 64, 8)];
	out->meta_stone = metadata_get(loaded.metadata, column_index(8, 64, 8));
	out->n_spawner = te.n_spawner;
	for(int i = 0; i < te.n_spawner; i++) {
		if(te.spawners[i].x == 2 && te.spawners[i].y == 64
		   && te.spawners[i].z == 2) {
			out->proven_present = true;
			strncpy(out->proven_mob_id, te.spawners[i].mob_id,
					sizeof(out->proven_mob_id) - 1);
			out->proven_delay = te.spawners[i].delay;
		}
		if(te.spawners[i].x == 5 && te.spawners[i].y == 64
		   && te.spawners[i].z == 5) {
			out->bare_present = true;
			strncpy(out->bare_mob_id, te.spawners[i].mob_id,
					sizeof(out->bare_mob_id) - 1);
			out->bare_delay = te.spawners[i].delay;
		}
	}
	tile_entities_parse_free(&te);
	server_chunk_arrays_free(&loaded);
	string_clear(world);
}

int main(void) {
	test_fixture_dir("test_mob_spawner_migrate_tmp", s_fixture_TMP_WORLD, sizeof s_fixture_TMP_WORLD);
	string_t world;
	string_init_set_str(world, TMP_WORLD);
	struct mob_spawner_te_migration_stats stats;

	// --- 1) One pass: only the TE-less spawner synthesizes ----------------
	fixture_create();
	assert(region_archive_migrate_mob_spawner_tes(world, &stats));
	assert(stats.regions_scanned == 1);
	assert(stats.chunks_scanned == 1);
	assert(stats.spawners_synthesized == 1); // ONLY the bare one

	struct chunk_state st;
	read_state(&st);
	assert(st.id_proven == BLOCK_MOB_SPAWNER); // untouched
	assert(st.meta_proven == 1);				// untouched, incl. the stale nibble
	assert(st.id_bare == BLOCK_MOB_SPAWNER);	// still a spawner
	assert(st.meta_bare == 0); // zeroed -- Beta's own faithful spawner-block value
	assert(st.id_stone == BLOCK_STONE && st.meta_stone == 2); // fully untouched
	assert(st.n_spawner == 2); // one TE gained, none lost

	assert(st.proven_present);
	assert(strcmp(st.proven_mob_id, "Skeleton") == 0); // pre-existing TE preserved verbatim
	assert(st.proven_delay == 137);

	assert(st.bare_present);
	assert(strcmp(st.bare_mob_id, "Zombie") == 0); // metadata 5&3==1 -> pickMobSpawner "Zombie"
	assert(st.bare_delay == 20); // Beta TileEntityMobSpawner() ctor default
	printf("[1] legacy-metadata spawner synthesized, TE-proven spawner and "
		   "unrelated block untouched\n");

	// --- 2) Idempotency -----------------------------------------------------
	assert(region_archive_migrate_mob_spawner_tes(world, &stats));
	assert(stats.spawners_synthesized == 0);
	struct chunk_state st2;
	read_state(&st2);
	assert(st2.n_spawner == 2);
	assert(st2.meta_bare == 0);
	assert(strcmp(st2.bare_mob_id, "Zombie") == 0);
	printf("[2] idempotent re-run is a no-op\n");

	fixture_remove();
	string_clear(world);
	printf("test_mob_spawner_migrate: ALL PASS\n");
	return 0;
}
