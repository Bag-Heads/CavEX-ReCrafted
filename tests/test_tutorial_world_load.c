/*
	Tutorial-world toolchain acceptance: a save written by the stdlib-only
	Python generator (tools/tutorial_world/worldgen_lib.py, demo scene
	tools/tutorial_world/demo_scene.py) must load through the REAL C readers
	-- region_archive_get_chunk for every chunk and cNBT for level.dat -- with
	the exact block ids, metadata, tile entities and precomputed light the
	generator meant. It then proves the game can WRITE those chunks back:
	a re-save of an unchanged chunk, and a re-save after the player breaks
	the chest (the native "deletion safe" path that compares the generator's
	Chest row byte-for-byte with tile_entities_build's own dump). Finally the
	"Tutorial Animals" fixture proves Beta mob rows from worldgen_lib.mob()
	load, and that a chunk re-saves after a mob moves (check_animals).

	usage: test_tutorial_world_load <dir containing "Tutorial Demo">
	The directory is written by tests/test_tutorial_worldgen_lib.py
	--fixture-out (ctest fixture `tutorial_world`). The save is COPIED into a
	pid-suffixed fixture dir first: region_archive_create opens "rb+".
*/

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0777)
#endif

#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "cNBT/nbt.h"
#include "entity/monsters_object.h"
#include "network/entity_archive.h"
#include "network/region_archive.h"
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"

#define CHECK(cond)                                                          \
	do {                                                                     \
		if(!(cond)) {                                                        \
			fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
					#cond);                                                  \
			exit(1);                                                         \
		}                                                                    \
	} while(0)

static char s_world[512];

static void copy_file(const char* from, const char* to) {
	FILE* in = fopen(from, "rb");
	if(!in) {
		fprintf(stderr, "cannot open %s (run the tutorial_world fixture)\n",
				from);
		exit(1);
	}
	FILE* out = fopen(to, "wb");
	CHECK(out);
	char buf[65536];
	size_t n;
	while((n = fread(buf, 1, sizeof buf, in)) > 0)
		CHECK(fwrite(buf, 1, n, out) == n);
	fclose(in);
	CHECK(fclose(out) == 0);
}

#define DEMO_CHUNKS 3 /* demo_scene.py: chunks (0..2, 0..2) */
static struct server_chunk s_sc[DEMO_CHUNKS][DEMO_CHUNKS];
static struct chunk_tile_entities s_te[DEMO_CHUNKS][DEMO_CHUNKS];
static struct chunk_entities s_ent[DEMO_CHUNKS][DEMO_CHUNKS];

static size_t cell(int x, int y, int z) {
	return ((size_t)(x & 15) << 11) | ((size_t)(z & 15) << 7) | (size_t)y;
}
static struct server_chunk* at(int x, int z) {
	CHECK(x >= 0 && z >= 0 && x < 16 * DEMO_CHUNKS && z < 16 * DEMO_CHUNKS);
	return &s_sc[x >> 4][z >> 4];
}
static int nib(const uint8_t* a, size_t i) {
	return (a[i >> 1] >> ((i & 1) * 4)) & 15;
}
static int id_at(int x, int y, int z) { return at(x, z)->ids[cell(x, y, z)]; }
static int meta_at(int x, int y, int z) {
	return nib(at(x, z)->metadata, cell(x, y, z));
}
static int sky_at(int x, int y, int z) {
	return nib(at(x, z)->lighting_sky, cell(x, y, z));
}
static int blk_at(int x, int y, int z) {
	return nib(at(x, z)->lighting_torch, cell(x, y, z));
}

static void check_level_dat(void) {
	char p[600];
	snprintf(p, sizeof p, "%s/level.dat", s_world);
	nbt_node* root = nbt_parse_path(p);
	CHECK(root);
	nbt_node* n = nbt_find_by_path(root, ".Data.CaveWiiSaveVersion");
	CHECK(n && n->type == TAG_INT && n->payload.tag_int == 5); /* no bed migration */
	n = nbt_find_by_path(root, ".Data.LevelName");
	CHECK(n && n->type == TAG_STRING
		  && !strcmp(n->payload.tag_string, "Tutorial Demo"));
	/* PLAYER-HEALTH-NBT: Health is Beta half-hearts (EntityLiving.java:558,
	 * full = 20); CaveWiiHealth carries the exact CaveWii units. 20 is
	 * exactly health_to_beta(160), so level_archive_read_player_health
	 * takes the marker and the tutorial player loads at full (160). */
	n = nbt_find_by_path(root, ".Data.Player.Health");
	CHECK(n && n->type == TAG_SHORT && n->payload.tag_short == 20);
	n = nbt_find_by_path(root, ".Data.Player.CaveWiiHealth");
	CHECK(n && n->type == TAG_SHORT && n->payload.tag_short == 160);
	n = nbt_find_by_path(root, ".Data.SpawnX");
	CHECK(n && n->type == TAG_INT && n->payload.tag_int == 21);
	n = nbt_find_by_path(root, ".Data.Player.Pos");
	CHECK(n && n->type == TAG_LIST);
	n = nbt_find_by_path(root, ".Data.Player.Inventory");
	CHECK(n && n->type == TAG_LIST);
	nbt_free(root);
}

/* The tutorial pen's animals: worldgen_lib.mob() writes Beta 1.7.3 mob rows
 * (fixture "Tutorial Animals", tests/test_tutorial_worldgen_lib.py
 * build_animals: chunk (0,0), a sign, Sheep red / Cow / Pig / Chicken at
 * x 4.5, 6.5, 8.5, 10.5, y 65, z 4.5). They must load, and a save after a
 * mob WALKS must succeed: a row the strict Beta import check
 * (entity_archive.c entity_beta_import_owned) rejects would fall back to
 * position matching and fail the moved mob (CEP_IMPORTED_UNMATCHED). */
static const struct archived_entity* find_mob(const struct chunk_entities* e,
											  int monster_id) {
	const struct archived_entity* hit = NULL;
	for(size_t i = 0; i < e->count; i++)
		if(e->entities[i].kind == ARCHIVED_ENTITY_MONSTER
		   && e->entities[i].data.monster.monster_id == monster_id) {
			CHECK(!hit);
			hit = &e->entities[i];
		}
	CHECK(hit);
	return hit;
}

static void load_animals(struct region_archive* ra, struct server_chunk* sc,
						 struct chunk_tile_entities* te,
						 struct chunk_entities* ent) {
	memset(sc, 0, sizeof *sc);
	memset(te, 0, sizeof *te);
	memset(ent, 0, sizeof *ent);
	CHECK(region_archive_get_chunk(ra, 0, 0, sc, te, ent));
	CHECK(ent->count == 4);
	CHECK(te->n_sign == 1);
}

static void check_animals(const char* fixture) {
	char world[512], dir[600], from[1024], to[1024];
	test_fixture_dir("tutorial_world_animals_tmp", world, sizeof world);
	MKDIR(world);
	snprintf(dir, sizeof dir, "%s/region", world);
	MKDIR(dir);
	snprintf(from, sizeof from, "%s/Tutorial Animals/level.dat", fixture);
	snprintf(to, sizeof to, "%s/level.dat", world);
	copy_file(from, to);
	snprintf(from, sizeof from, "%s/Tutorial Animals/region/r.0.0.mcr",
			 fixture);
	snprintf(to, sizeof to, "%s/region/r.0.0.mcr", world);
	copy_file(from, to);

	string_t level;
	string_init_set_str(level, world);
	struct region_archive ra;
	CHECK(region_archive_create(&ra, level, 0, 0, WORLD_DIM_OVERWORLD));
	struct server_chunk sc;
	struct chunk_tile_entities te;
	struct chunk_entities ent;
	load_animals(&ra, &sc, &te, &ent);
	CHECK(!sc.persisted_tile_entities_complete); /* pure-Beta mob chunk */
	const struct archived_entity* sheep = find_mob(&ent, MONSTER_SHEEP);
	CHECK(sheep->data.monster.fleece_color == 14 && !sheep->data.monster.sheared);
	CHECK(sheep->health == 10 && sheep->pos[0] == 4.5f && sheep->pos[1] == 65.0f);
	CHECK(find_mob(&ent, MONSTER_CHICKEN)->health == 4);
	CHECK(!find_mob(&ent, MONSTER_PIG)->data.monster.saddled);
	struct archived_entity* cow
		= (struct archived_entity*)find_mob(&ent, MONSTER_COW);
	CHECK(cow->pos[0] == 6.5f && cow->pos[2] == 4.5f && cow->orient[0] == 90.0f);

	/* Two walks, two saves: the first rewrites the imported Beta rows, the
	 * second rewrites the game's own rows. Each reload must hold exactly the
	 * four mobs with the cow where it walked to. */
	const float walk[2][2] = {{9.25f, 11.75f}, {13.5f, 2.25f}};
	for(int k = 0; k < 2; k++) {
		cow->pos[0] = walk[k][0];
		cow->pos[2] = walk[k][1];
		/* The game's snapshot carries each live mob's runtime id
		 * (server_local.c: out->source_id = e->id, never 0), fresh after
		 * every load, so remap them per save as a reload would. */
		for(size_t i = 0; i < ent.count; i++)
			ent.entities[i].source_id = (uint32_t)(100 * (k + 1) + i + 1);
		bool saved = region_archive_set_chunk(&ra, 0, 0, &sc, &te, &ent);
		if(!saved)
			fprintf(stderr, "moved-mob save %d failed: stage=%s sub=%u\n", k,
					region_save_failure_stage_name(
						region_save_failure_stage()),
					(unsigned)region_save_failure_sub());
		CHECK(saved);
		tile_entities_parse_free(&te);
		chunk_entities_free(&ent);
		server_chunk_arrays_free(&sc);
		load_animals(&ra, &sc, &te, &ent);
		cow = (struct archived_entity*)find_mob(&ent, MONSTER_COW);
		CHECK(cow->pos[0] == walk[k][0] && cow->pos[2] == walk[k][1]
			  && cow->pos[1] == 65.0f);
		sheep = find_mob(&ent, MONSTER_SHEEP);
		CHECK(sheep->pos[0] == 4.5f && sheep->data.monster.fleece_color == 14);
		CHECK(find_mob(&ent, MONSTER_PIG)->pos[0] == 8.5f);
		CHECK(find_mob(&ent, MONSTER_CHICKEN)->pos[0] == 10.5f);
	}
	tile_entities_parse_free(&te);
	chunk_entities_free(&ent);
	server_chunk_arrays_free(&sc);
	CHECK(region_archive_destroy(&ra));
	string_clear(level);
	test_fixture_dir_cleanup(world);
}

/* ---- --real: the SHIPPED tutorial world ---------------------------------
 * The world tools/tutorial_world/build_tutorial.py writes (and
 * tools/make_release.sh puts in the release zip), built into the ctest
 * fixture dir by test_tutorial_world_real_build with --summary. Through the
 * production reader/writer: every chunk of r.0.0.mcr loads; every sign and
 * chest tile entity sits on its block, signs carry text, chests carry items,
 * and the C reader's counts equal the builder's own summary; one pen animal
 * moves and its chunk saves and reloads; one chest is broken and its chunk
 * re-saves twice (edit, then an unchanged rewrite of the game's own rows). */
#define REAL_WORLD "CavEX Tutorial" /* build_tutorial.py DEFAULT_OUT dir */
#define REAL_CHUNKS 32				  /* one region: chunks (0..31, 0..31) */

static long summary_value(const char* path, const char* key) {
	FILE* f = fopen(path, "r");
	CHECK(f);
	char k[64];
	long v;
	while(fscanf(f, "%63s %ld", k, &v) == 2)
		if(!strcmp(k, key)) {
			fclose(f);
			return v;
		}
	fclose(f);
	fprintf(stderr, "summary %s has no '%s'\n", path, key);
	exit(1);
}

static void real_free(struct server_chunk* sc, struct chunk_tile_entities* te,
					  struct chunk_entities* ent) {
	tile_entities_parse_free(te);
	chunk_entities_free(ent);
	server_chunk_arrays_free(sc);
}

static void real_load(struct region_archive* ra, int cx, int cz,
					  struct server_chunk* sc, struct chunk_tile_entities* te,
					  struct chunk_entities* ent) {
	memset(sc, 0, sizeof *sc);
	memset(te, 0, sizeof *te);
	memset(ent, 0, sizeof *ent);
	CHECK(region_archive_get_chunk(ra, cx, cz, sc, te, ent));
}

static int real_id(const struct server_chunk* sc, int x, int y, int z) {
	CHECK(y >= 0 && y < 128);
	return sc->ids[cell(x, y, z)];
}

static size_t find_cow(const struct chunk_entities* e) {
	for(size_t i = 0; i < e->count; i++)
		if(e->entities[i].kind == ARCHIVED_ENTITY_MONSTER
		   && e->entities[i].data.monster.monster_id == MONSTER_COW)
			return i;
	return (size_t)-1;
}

static void check_real_world(const char* fixture) {
	char world[512], dir[600], from[1024], to[1024], summary[1024];
	snprintf(summary, sizeof summary, "%s/summary.txt", fixture);
	test_fixture_dir("tutorial_world_real_tmp", world, sizeof world);
	MKDIR(world);
	snprintf(dir, sizeof dir, "%s/region", world);
	MKDIR(dir);
	snprintf(from, sizeof from, "%s/" REAL_WORLD "/level.dat", fixture);
	snprintf(to, sizeof to, "%s/level.dat", world);
	copy_file(from, to);
	snprintf(from, sizeof from, "%s/" REAL_WORLD "/region/r.0.0.mcr", fixture);
	snprintf(to, sizeof to, "%s/region/r.0.0.mcr", world);
	copy_file(from, to);

	/* level.dat through cNBT: the name the world list shows, and the spawn
	 * on the landing beach (test_tutorial_build.py test_region_and_level). */
	snprintf(to, sizeof to, "%s/level.dat", world);
	nbt_node* root = nbt_parse_path(to);
	CHECK(root);
	nbt_node* n = nbt_find_by_path(root, ".Data.LevelName");
	CHECK(n && n->type == TAG_STRING
		  && !strcmp(n->payload.tag_string, "Tutorial Island"));
	n = nbt_find_by_path(root, ".Data.SpawnX");
	CHECK(n && n->type == TAG_INT && n->payload.tag_int == 200);
	n = nbt_find_by_path(root, ".Data.SpawnY");
	CHECK(n && n->type == TAG_INT && n->payload.tag_int == 65);
	n = nbt_find_by_path(root, ".Data.SpawnZ");
	CHECK(n && n->type == TAG_INT && n->payload.tag_int == 304);
	n = nbt_find_by_path(root, ".Data.CaveWiiSaveVersion");
	CHECK(n && n->type == TAG_INT);
	n = nbt_find_by_path(root, ".Data.Player.Pos");
	CHECK(n && n->type == TAG_LIST);
	nbt_free(root);

	string_t level;
	string_init_set_str(level, world);
	struct region_archive ra;
	CHECK(region_archive_create(&ra, level, 0, 0, WORLD_DIM_OVERWORLD));

	/* 1) Every chunk loads; every sign/chest sits on its block. */
	long chunks = 0, signs = 0, chests = 0, mobs = 0;
	int cow_cx = -1, cow_cz = -1, chest_cx = -1, chest_cz = -1;
	struct server_chunk sc;
	struct chunk_tile_entities te;
	struct chunk_entities ent;
	for(int cx = 0; cx < REAL_CHUNKS; cx++)
		for(int cz = 0; cz < REAL_CHUNKS; cz++) {
			bool exists = false;
			CHECK(region_archive_contains(&ra, cx, cz, &exists));
			if(!exists)
				continue;
			real_load(&ra, cx, cz, &sc, &te, &ent);
			CHECK(sc.populated);
			chunks++;
			for(int i = 0; i < te.n_sign; i++) {
				const struct te_sign* s = &te.signs[i];
				CHECK((s->x >> 4) == cx && (s->z >> 4) == cz);
				int id = real_id(&sc, s->x, s->y, s->z);
				CHECK(id == 63 || id == 68);
				bool text = false;
				for(int k = 0; k < TE_SIGN_SIZE && s->text[k]; k++)
					text |= s->text[k] != ' ';
				CHECK(text);
			}
			for(int i = 0; i < te.n_chest; i++) {
				const struct te_chest* c = &te.chests[i];
				CHECK((c->x >> 4) == cx && (c->z >> 4) == cz);
				CHECK(real_id(&sc, c->x, c->y, c->z) == 54);
				bool any = false;
				for(int k = 0; k < TE_CHEST_SLOTS; k++)
					any |= c->items[k].id != 0 && c->items[k].count > 0;
				CHECK(any);
			}
			signs += te.n_sign;
			chests += te.n_chest;
			mobs += (long)ent.count;
			if(cow_cx < 0 && find_cow(&ent) != (size_t)-1) {
				cow_cx = cx;
				cow_cz = cz;
			}
			if(chest_cx < 0 && te.n_chest > 0) {
				chest_cx = cx;
				chest_cz = cz;
			}
			real_free(&sc, &te, &ent);
		}
	CHECK(chunks == summary_value(summary, "chunks"));
	CHECK(signs == summary_value(summary, "signs"));
	CHECK(chests == summary_value(summary, "chests"));
	CHECK(mobs == summary_value(summary, "mobs"));
	CHECK(signs > 0 && chests > 0 && mobs > 0 && cow_cx >= 0 && chest_cx >= 0);

	/* 2) Move one pen animal (a cow) inside its chunk; save; reload. */
	real_load(&ra, cow_cx, cow_cz, &sc, &te, &ent);
	size_t n_ent = ent.count;
	int n_sign = te.n_sign, n_chest = te.n_chest;
	struct archived_entity* cow = &ent.entities[find_cow(&ent)];
	const float to_x = (float)(cow_cx * 16) + 7.25f;
	const float to_z = (float)(cow_cz * 16) + 9.75f;
	const float cow_y = cow->pos[1];
	cow->pos[0] = to_x;
	cow->pos[2] = to_z;
	for(size_t i = 0; i < ent.count; i++) /* fresh runtime ids, as a reload */
		ent.entities[i].source_id = (uint32_t)(500 + i + 1);
	bool saved = region_archive_set_chunk(&ra, cow_cx, cow_cz, &sc, &te, &ent);
	if(!saved)
		fprintf(stderr, "real moved-mob save failed: stage=%s sub=%u\n",
				region_save_failure_stage_name(region_save_failure_stage()),
				(unsigned)region_save_failure_sub());
	CHECK(saved);
	real_free(&sc, &te, &ent);
	real_load(&ra, cow_cx, cow_cz, &sc, &te, &ent);
	CHECK(ent.count == n_ent && te.n_sign == n_sign && te.n_chest == n_chest);
	bool found = false;
	for(size_t i = 0; i < ent.count; i++) {
		const struct archived_entity* e = &ent.entities[i];
		if(e->kind == ARCHIVED_ENTITY_MONSTER
		   && e->data.monster.monster_id == MONSTER_COW && e->pos[0] == to_x
		   && e->pos[2] == to_z && e->pos[1] == cow_y)
			found = true;
	}
	CHECK(found);
	real_free(&sc, &te, &ent);

	/* 3) Break one chest (block -> air, its tile entity gone); save; reload;
	 *    then rewrite the reloaded chunk unchanged and reload again. */
	real_load(&ra, chest_cx, chest_cz, &sc, &te, &ent);
	n_sign = te.n_sign;
	n_chest = te.n_chest;
	n_ent = ent.count;
	const struct te_chest broken = te.chests[0];
	sc.ids[cell(broken.x, broken.y, broken.z)] = 0;
	struct chunk_tile_entities edited = te;
	edited.chests = n_chest > 1 ? te.chests + 1 : NULL;
	edited.n_chest = n_chest - 1;
	CHECK(region_archive_set_chunk(&ra, chest_cx, chest_cz, &sc, &edited, &ent));
	real_free(&sc, &te, &ent);
	for(int pass = 0; pass < 2; pass++) {
		real_load(&ra, chest_cx, chest_cz, &sc, &te, &ent);
		CHECK(te.n_chest == n_chest - 1 && te.n_sign == n_sign
			  && ent.count == n_ent);
		CHECK(real_id(&sc, broken.x, broken.y, broken.z) == 0);
		for(int i = 0; i < te.n_chest; i++)
			CHECK(!(te.chests[i].x == broken.x && te.chests[i].y == broken.y
					&& te.chests[i].z == broken.z));
		if(pass == 0)
			CHECK(region_archive_set_chunk(&ra, chest_cx, chest_cz, &sc, &te,
										   &ent));
		real_free(&sc, &te, &ent);
	}

	CHECK(region_archive_destroy(&ra));
	string_clear(level);
	test_fixture_dir_cleanup(world);
	printf("test_tutorial_world_load --real: PASS (%ld chunks, %ld signs, %ld "
		   "chests, %ld mobs; cow moved in chunk %d,%d; chest broken at "
		   "%d,%d,%d + re-save)\n",
		   chunks, signs, chests, mobs, cow_cx, cow_cz, broken.x, broken.y,
		   broken.z);
}

int main(int argc, char** argv) {
	if(argc == 3 && !strcmp(argv[1], "--real")) {
		check_real_world(argv[2]);
		return 0;
	}
	CHECK(argc == 2);
	test_fixture_dir("tutorial_world_load_tmp", s_world, sizeof s_world);
	MKDIR(s_world);
	char region_dir[600], from[1024], to[1024];
	snprintf(region_dir, sizeof region_dir, "%s/region", s_world);
	MKDIR(region_dir);
	snprintf(from, sizeof from, "%s/Tutorial Demo/level.dat", argv[1]);
	snprintf(to, sizeof to, "%s/level.dat", s_world);
	copy_file(from, to);
	snprintf(from, sizeof from, "%s/Tutorial Demo/region/r.0.0.mcr", argv[1]);
	snprintf(to, sizeof to, "%s/region/r.0.0.mcr", s_world);
	copy_file(from, to);

	check_level_dat();

	string_t level;
	string_init_set_str(level, s_world);
	struct region_archive ra;
	CHECK(region_archive_create(&ra, level, 0, 0, WORLD_DIM_OVERWORLD));

	/* 1) Every chunk loads through the production reader. */
	for(int cx = 0; cx < DEMO_CHUNKS; cx++)
		for(int cz = 0; cz < DEMO_CHUNKS; cz++) {
			bool exists = false;
			CHECK(region_archive_contains(&ra, cx, cz, &exists) && exists);
			CHECK(region_archive_get_chunk(&ra, cx, cz, &s_sc[cx][cz],
										   &s_te[cx][cz], &s_ent[cx][cz]));
			CHECK(s_sc[cx][cz].populated);
			CHECK(s_sc[cx][cz].persisted_tile_entities_complete);
			CHECK(s_te[cx][cz].legacy_flat_complete);
			CHECK(s_ent[cx][cz].count == 0);
		}
	bool exists = true;
	CHECK(region_archive_contains(&ra, 3, 0, &exists) && !exists);

	/* 2) Blocks + metadata (encodings documented in worldgen_lib.py). */
	CHECK(id_at(0, 0, 0) == 7);                      /* bedrock */
	CHECK(id_at(24, 64, 24 + 13) == 2);              /* island grass */
	CHECK(id_at(1, 63, 1) == 9 && meta_at(1, 63, 1) == 0); /* sea */
	CHECK(id_at(22, 66, 24) == 68 && meta_at(22, 66, 24) == 3); /* wall sign S */
	CHECK(id_at(21, 65, 23) == 64 && meta_at(21, 65, 23) == 3); /* door low */
	CHECK(id_at(21, 66, 23) == 64 && meta_at(21, 66, 23) == 11); /* door top */
	CHECK(id_at(19, 67, 21) == 50 && meta_at(19, 67, 21) == 1); /* torch E */
	CHECK(id_at(23, 65, 19) == 62 && meta_at(23, 65, 19) == 3); /* lit furnace */
	CHECK(id_at(19, 65, 19) == 54);
	CHECK(id_at(20, 69, 17) == 53 && meta_at(20, 69, 17) == 3); /* stairs N */
	CHECK(id_at(20, 69, 24) == 53 && meta_at(20, 69, 24) == 2); /* stairs S */
	CHECK(id_at(23, 65, 21) == 26 && meta_at(23, 65, 21) == 2); /* bed foot, Beta dir 2 = north */
	CHECK(id_at(23, 65, 20) == 26 && meta_at(23, 65, 20) == 10); /* bed head = 2 | 8 */
	CHECK(id_at(14, 65, 28) == 69 && meta_at(14, 65, 28) == 2); /* lever off */
	CHECK(id_at(16, 65, 28) == 76 && meta_at(16, 65, 28) == 1); /* rs torch */
	CHECK(id_at(17, 65, 28) == 55 && meta_at(17, 65, 28) == 15);
	CHECK(id_at(19, 65, 28) == 94 && meta_at(19, 65, 28) == (1 | (1 << 2)));
	CHECK(id_at(22, 65, 28) == 55 && meta_at(22, 65, 28) == 13);
	for(int s = 0; s < 8; s++) {
		CHECK(id_at(18 + s, 65, 30) == 59 && meta_at(18 + s, 65, 30) == s);
		CHECK(id_at(18 + s, 64, 30) == 60 && meta_at(18 + s, 64, 30) == 7);
	}
	CHECK(id_at(21, 64, 31) == 9);

	/* 3) Light, as stored (the loader never relights): Beta's converged
	 *    values with the game's own emission/opacity table. */
	CHECK(sky_at(30, 65, 30) == 15 && blk_at(30, 65, 30) == 0); /* open air */
	CHECK(sky_at(2, 63, 2) == 12 && sky_at(2, 62, 2) == 9);     /* water -3 */
	CHECK(sky_at(24, 30, 24) == 0 && blk_at(24, 30, 24) == 0);  /* deep stone */
	CHECK(blk_at(19, 67, 21) == 14 && blk_at(20, 67, 21) == 13); /* torch */
	CHECK(blk_at(23, 65, 19) == 13);                             /* furnace */
	CHECK(blk_at(16, 65, 28) == 7);                              /* rs torch */
	CHECK(at(30, 30)->heightmap[(30 & 15) << 4 | (30 & 15)] == 65);
	CHECK(at(2, 2)->heightmap[(2 << 4) | 2] == 64);

	/* 4) Tile entities. */
	const struct chunk_tile_entities* te = &s_te[1][1];
	CHECK(te->n_sign == 1 && te->signs[0].x == 22 && te->signs[0].y == 66
		  && te->signs[0].z == 24);
	CHECK(!strncmp(te->signs[0].text, "Hello           ", 16));
	CHECK(!strncmp(te->signs[0].text + 16, "CaveWii         ", 16));
	CHECK(te->n_chest == 1 && te->chests[0].x == 19);
	CHECK(te->chests[0].items[0].id == 270 && te->chests[0].items[0].count == 1);
	CHECK(te->chests[0].items[1].id == 50 && te->chests[0].items[1].count == 16);
	CHECK(te->chests[0].items[2].id == 297 && te->chests[0].items[2].count == 3);
	for(int k = 0; k < 3; k++) /* canonical item bytes, no retained record */
		CHECK(te->chests[0].items[k].nbt_record == 0);
	CHECK(te->chests[0].items[3].id == 0);
	CHECK(te->n_furnace == 1 && te->furnaces[0].burn_time == 1200
		  && te->furnaces[0].cook_time == 50);
	CHECK(te->furnaces[0].items[0].id == 15 && te->furnaces[0].items[0].count == 4);
	CHECK(te->furnaces[0].items[1].id == 263 && te->furnaces[0].items[1].count == 2);

	/* 5) The game can rewrite a generated chunk: unchanged, then with the
	 *    chest broken (the byte-exact native deletion-safe path). */
	struct server_chunk* sc = &s_sc[1][1];
	struct chunk_tile_entities edited = s_te[1][1];
	CHECK(region_archive_set_chunk(&ra, 1, 1, sc, &edited, &s_ent[1][1]));
	sc->ids[cell(19, 65, 19)] = 0;
	edited.n_chest = 0;
	edited.chests = NULL;
	CHECK(region_archive_set_chunk(&ra, 1, 1, sc, &edited, &s_ent[1][1]));
	struct server_chunk back = {0};
	struct chunk_tile_entities back_te = {0};
	struct chunk_entities back_ent = {0};
	CHECK(region_archive_get_chunk(&ra, 1, 1, &back, &back_te, &back_ent));
	CHECK(back_te.n_chest == 0 && back_te.n_sign == 1 && back_te.n_furnace == 1);
	CHECK(back.ids[cell(19, 65, 19)] == 0 && back.ids[cell(23, 65, 19)] == 62);
	tile_entities_parse_free(&back_te);
	chunk_entities_free(&back_ent);
	server_chunk_arrays_free(&back);

	for(int cx = 0; cx < DEMO_CHUNKS; cx++)
		for(int cz = 0; cz < DEMO_CHUNKS; cz++) {
			tile_entities_parse_free(&s_te[cx][cz]);
			chunk_entities_free(&s_ent[cx][cz]);
			server_chunk_arrays_free(&s_sc[cx][cz]);
		}
	CHECK(region_archive_destroy(&ra));
	string_clear(level);
	test_fixture_dir_cleanup(s_world);

	check_animals(argv[1]);
	printf("test_tutorial_world_load: PASS (9 chunks, level.dat, light, TEs, "
		   "re-save; 4 Beta mobs, moved-mob save x2)\n");
	return 0;
}
