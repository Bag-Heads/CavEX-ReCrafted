/*
	PLAYER-HEALTH-NBT (2026-10-25, beta-trial-1025). level.dat's
	Data.Player.Health must be Beta's own unit: EntityLiving.health in
	HALF-HEARTS, full = 20 (EntityPlayer.java:47 sets health to 20), written
	as a short tag "Health" (EntityLiving.java:558) and
	read back with getShort("Health") (EntityLiving.java:565).
	CaveWii's internal scale is 8 units per half-heart (MAX_PLAYER_HEALTH 160).
	The pre-fix build wrote/read the internal value verbatim: a real b1.7.3
	client loaded a CaveWii player at 6x-8x health, and an imported Beta
	world's 20 loaded as 1.25 hearts.

	Cases (production level_archive + the production save-version chain):
	  1. writer: full / half health -> Health 20 / 10, TAG_SHORT on disk;
	  2. a real Beta level.dat (no CaveWii* tags) Health 20 -> 160, 7 -> 56,
	     and the decision survives the chain's v5 stamp + reopen;
	  3. a pre-fix CaveWii save (CaveWiiSaveVersion, Health 160, no marker)
	     loads full (not 8x) and is persisted as Health 20; also a pre-version
	     CaveWii world (CaveWiiGenerator only);
	  4. round trip of exact units incl. 0 (dead) and a non-multiple of 8;
	  3b. BLOCKER 1: a pre-tag CaveWii world (no CaveWii* tag) holding units
	      16 loads 16, not 128; a template raw 20 resolves to full;
	  3c. BLOCKER 2: a Beta world a pre-fix build stamped CaveWiiSaveVersion
	      5 without a checkpoint loads 20 -> full, 7 -> 56; > 20 -> units;
	  5. an external edit of Health beats a stale CaveWiiHealth marker;
	  6. the split-screen guest reader keeps pre-fix CaveWii-only records.
	Each case is a CHECK (non-aborting) so a run on the pre-fix code reports
	every failing case. Build with -DPLAYER_HEALTH_OLD_API against the pre-fix
	level_archive.c to reproduce the old behaviour (the old checkpoint wrote
	LEVEL_PLAYER_HEALTH raw, and the old chain never touched Health).
*/
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0777)
#endif
#include <m-lib/m-string.h>

#include "support/test_fixture_dir.h"
#include "cNBT/list.h"
#include "cNBT/nbt.h"
#include "network/level_archive.h"
#include "network/server_interface.h"

#ifdef PLAYER_HEALTH_OLD_API
/* Pre-fix production behaviour, expressed through the same seams. */
void inventory_clear(struct inventory* inv) {
	for(size_t k = 0; k < inv->capacity; k++)
		inv->items[k] = (struct item_data) {0};
	inv->picked_item = (struct item_data) {0};
	inv->hotbar_slot = 0;
}
static bool write_health(struct level_archive* la, int16_t units) {
	return level_archive_write(la, LEVEL_PLAYER_HEALTH, &units);
}
static bool run_chain(struct level_archive* la, string_t world) {
	(void)la;
	(void)world;
	return true; /* the pre-fix chain had no Health step */
}
static bool guest_read(struct level_archive* la, int16_t* out) {
	return level_archive_read(la, LEVEL_PLAYER_HEALTH, out, 0);
}
#elif defined(PLAYER_HEALTH_LEAN)
/* Lean link (no server_local): the chain's only Health step is
 * level_archive_normalize_player_health, called first in the chain. */
void inventory_clear(struct inventory* inv) {
	for(size_t k = 0; k < inv->capacity; k++)
		inv->items[k] = (struct item_data) {0};
	inv->picked_item = (struct item_data) {0};
	inv->hotbar_slot = 0;
}
static bool write_health(struct level_archive* la, int16_t units) {
	return level_archive_write_player_health(la, units);
}
static bool run_chain(struct level_archive* la, string_t world) {
	(void)world;
	return level_archive_normalize_player_health(la, NULL);
}
static bool guest_read(struct level_archive* la, int16_t* out) {
	return level_archive_read_player_health_strict(la, true, out);
}
#else
#include "network/server_local.h"
static bool write_health(struct level_archive* la, int16_t units) {
	return level_archive_write_player_health(la, units);
}
static bool run_chain(struct level_archive* la, string_t world) {
	return server_local_save_version_chain(la, world, false, 0);
}
static bool guest_read(struct level_archive* la, int16_t* out) {
	return level_archive_read_player_health_strict(la, true, out);
}
#endif

static int s_fail;
#define CHECK(c)                                                               \
	do {                                                                       \
		if(!(c)) {                                                             \
			fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);       \
			s_fail++;                                                          \
		}                                                                      \
	} while(0)

static char s_dir[160];

static void remove_named(nbt_node* parent, const char* name) {
	if(!parent || parent->type != TAG_COMPOUND)
		return;
	struct list_head* cur;
	struct list_head* tmp;
	list_for_each_safe(cur, tmp, &parent->payload.tag_compound->entry) {
		struct nbt_list* e = list_entry(cur, struct nbt_list, entry);
		if(e->data && e->data->name && !strcmp(e->data->name, name)) {
			list_del(cur);
			nbt_free(e->data);
			free(e);
		}
	}
}

static void set_raw_health(struct level_archive* la, int16_t v) {
	nbt_node* h = nbt_find_by_path(la->data, ".Data.Player.Health");
	assert(h && h->type == TAG_SHORT);
	h->payload.tag_short = v;
	la->modified = true;
}

/* A fresh world dir (level.dat + empty region/). */
static void fresh_world(string_t world) {
	char region[200];
	MKDIR(s_dir);
	snprintf(region, sizeof region, "%s/region", s_dir);
	MKDIR(region);
	char p[200];
	snprintf(p, sizeof p, "%s/level.dat", s_dir); remove(p);
	snprintf(p, sizeof p, "%s/level.dat_old", s_dir); remove(p);
	snprintf(p, sizeof p, "%s/level.dat_new", s_dir); remove(p);
	struct level_archive la;
	assert(level_archive_create_new(&la, world, 7, (vec3) {10, 70, 20}));
	assert(level_archive_save(&la));
	level_archive_destroy(&la);
}

/* Shape the on-disk level.dat: a real Beta file (strip every CaveWii* tag)
 * or a pre-fix CaveWii file (strip only the new marker), with raw Health. */
/* Append a scalar child (Beta-client-only Player tags). */
static void add_scalar(nbt_node* parent, const char* name, nbt_type type,
					   int32_t v) {
	nbt_node* n = calloc(1, sizeof(*n));
	struct nbt_list* e = calloc(1, sizeof(*e));
	assert(n && e);
	n->name = malloc(strlen(name) + 1);
	assert(n->name);
	strcpy(n->name, name);
	n->type = type;
	if(type == TAG_INT) n->payload.tag_int = v;
	else if(type == TAG_SHORT) n->payload.tag_short = (int16_t)v;
	else n->payload.tag_byte = (int8_t)v;
	e->data = n;
	list_add_tail(&e->entry, &parent->payload.tag_compound->entry);
}

/* SHAPE_BETA: a real Beta 1.7.3 level.dat (no CaveWii* tags; Player carries
 *   Beta's unconditional Sleeping/SleepTimer/Score).
 * SHAPE_BETA_STAMPED: blocker 2 -- that Beta world after a PRE-fix CaveWii
 *   load stamped CaveWiiSaveVersion 5, with no checkpoint yet.
 * SHAPE_PREFIX_CAVEWII: a CaveWii-created world saved by a pre-fix build.
 * SHAPE_PREVERSION_CAVEWII: same, before CaveWiiSaveVersion (generator only).
 * SHAPE_ERA0_CAVEWII: blocker 1 -- a CaveWii-created world from before both
 *   CaveWiiGenerator and CaveWiiSaveVersion existed (no CaveWii tag at all). */
enum shape { SHAPE_BETA, SHAPE_BETA_STAMPED, SHAPE_PREFIX_CAVEWII,
	SHAPE_PREVERSION_CAVEWII, SHAPE_ERA0_CAVEWII };
static void shape_world(string_t world, enum shape shape, int16_t raw) {
	struct level_archive la;
	assert(level_archive_create(&la, world));
	nbt_node* data = nbt_find_by_path(la.data, ".Data");
	nbt_node* player = nbt_find_by_path(la.data, ".Data.Player");
	remove_named(player, "CaveWiiHealth");
	if(shape == SHAPE_BETA || shape == SHAPE_BETA_STAMPED
	   || shape == SHAPE_ERA0_CAVEWII) {
		remove_named(data, "CaveWiiGenerator");
		remove_named(data, "CaveWiiDeathTxCounter");
		if(shape != SHAPE_BETA_STAMPED)
			remove_named(data, "CaveWiiSaveVersion");
	} else if(shape == SHAPE_PREVERSION_CAVEWII) {
		remove_named(data, "CaveWiiSaveVersion");
	}
	if(shape == SHAPE_BETA || shape == SHAPE_BETA_STAMPED) {
		add_scalar(player, "Sleeping", TAG_BYTE, 0);
		add_scalar(player, "SleepTimer", TAG_SHORT, 0);
		add_scalar(player, "Score", TAG_INT, 0);
	}
	set_raw_health(&la, raw);
	la.modified = true;
	assert(level_archive_save(&la));
	level_archive_destroy(&la);
}

/* World load as production does it: open, chain, read. */
static int16_t load_health(string_t world, bool save_after) {
	struct level_archive la;
	assert(level_archive_create(&la, world));
	CHECK(run_chain(&la, world));
	int16_t h = -1234;
	CHECK(level_archive_read_player_health(&la, &h));
	if(save_after) {
		/* the next checkpoint (server_local_write_level_state) */
		CHECK(write_health(&la, h));
		CHECK(level_archive_save(&la));
	}
	level_archive_discard(&la);
	return h;
}

/* Disk Health: present, TAG_SHORT, value. */
static bool disk_health(string_t world, int16_t* out) {
	struct level_archive la;
	assert(level_archive_create(&la, world));
	nbt_node* h = nbt_find_by_path(la.data, ".Data.Player.Health");
	bool ok = h && h->type == TAG_SHORT;
	if(ok)
		*out = h->payload.tag_short;
	level_archive_discard(&la);
	return ok;
}

int main(int argc, char** argv) {
	if(argc == 2) {
		/* manual: load an existing world dir (e.g. the generated tutorial
		 * world) through the production chain + reader, without saving */
		string_t w;
		string_init_set_str(w, argv[1]);
		int16_t h = load_health(w, false);
		printf("world %s loads Health %d (MAX %d)\n", argv[1], h,
			   MAX_PLAYER_HEALTH);
		string_clear(w);
		return h == MAX_PLAYER_HEALTH && !s_fail ? 0 : 1;
	}
	test_fixture_dir("test_player_nbt_beta_types_tmp", s_dir, sizeof s_dir);
	MKDIR(s_dir);
	string_t world;
	string_init_set_str(world, s_dir);
	int16_t d = 0;

	/* 1. writer: full -> 20, half -> 10, TAG_SHORT (a fresh world is full). */
	fresh_world(world);
	CHECK(disk_health(world, &d) && d == 20);
	{
		struct level_archive la;
		assert(level_archive_create(&la, world));
		CHECK(write_health(&la, MAX_PLAYER_HEALTH));
		CHECK(level_archive_save(&la));
		level_archive_destroy(&la);
	}
	CHECK(disk_health(world, &d) && d == 20);
	{
		struct level_archive la;
		assert(level_archive_create(&la, world));
		CHECK(write_health(&la, MAX_PLAYER_HEALTH / 2));
		CHECK(level_archive_save(&la));
		level_archive_destroy(&la);
	}
	CHECK(disk_health(world, &d) && d == 10);

	/* 2. real Beta level.dat: Health 20 -> full, 7 -> 56 units. */
	fresh_world(world);
	shape_world(world, SHAPE_BETA, 20);
	CHECK(load_health(world, false) == MAX_PLAYER_HEALTH);
	/* the chain stamped CaveWiiSaveVersion and persisted; a crash before any
	 * checkpoint must not flip the decision to "CaveWii units" next load */
	CHECK(load_health(world, false) == MAX_PLAYER_HEALTH);
	fresh_world(world);
	shape_world(world, SHAPE_BETA, 7);
	CHECK(load_health(world, true) == 7 * 8);
	CHECK(disk_health(world, &d) && d == 7);
	CHECK(load_health(world, false) == 7 * 8);

	/* 3. pre-fix CaveWii save holding 160: full, not 8x; persisted as 20. */
	fresh_world(world);
	shape_world(world, SHAPE_PREFIX_CAVEWII, MAX_PLAYER_HEALTH);
	CHECK(load_health(world, true) == MAX_PLAYER_HEALTH);
	CHECK(disk_health(world, &d) && d == 20);
	CHECK(load_health(world, false) == MAX_PLAYER_HEALTH);
	fresh_world(world);
	shape_world(world, SHAPE_PREFIX_CAVEWII, 56); /* 3.5 hearts */
	CHECK(load_health(world, true) == 56);
	CHECK(disk_health(world, &d) && d == 7);
	fresh_world(world);
	shape_world(world, SHAPE_PREVERSION_CAVEWII, MAX_PLAYER_HEALTH);
	CHECK(load_health(world, true) == MAX_PLAYER_HEALTH);
	CHECK(disk_health(world, &d) && d == 20);

	/* 3b. BLOCKER 1: pre-tag CaveWii world (template-born Player, no
	 * CaveWii* tag) holding internal units 16 = 1 heart: 16, not 128. */
	fresh_world(world);
	shape_world(world, SHAPE_ERA0_CAVEWII, 16);
	CHECK(load_health(world, true) == 16);
	CHECK(disk_health(world, &d) && d == 2);
	CHECK(load_health(world, false) == 16);
	/* ambiguous by design: a template-born raw 20 (2026-05-31..07-27
	 * template, never checkpointed) resolves to full health, not 1/8 */
	fresh_world(world);
	shape_world(world, SHAPE_ERA0_CAVEWII, 20);
	CHECK(load_health(world, false) == MAX_PLAYER_HEALTH);

	/* 3c. BLOCKER 2: Beta world stamped CaveWiiSaveVersion=5 by a pre-fix
	 * build, not yet checkpointed: Beta 20 -> full, Beta 7 -> 56, and the
	 * decision is durable. */
	fresh_world(world);
	shape_world(world, SHAPE_BETA_STAMPED, 20);
	CHECK(load_health(world, true) == MAX_PLAYER_HEALTH);
	CHECK(disk_health(world, &d) && d == 20);
	CHECK(load_health(world, false) == MAX_PLAYER_HEALTH);
	fresh_world(world);
	shape_world(world, SHAPE_BETA_STAMPED, 7);
	CHECK(load_health(world, false) == 7 * 8);
	/* the same import after a pre-fix checkpoint above Beta's cap (heal()
	 * caps at 20): only CaveWii units can exceed it -> 120 units exact */
	fresh_world(world);
	shape_world(world, SHAPE_BETA_STAMPED, 120);
	CHECK(load_health(world, true) == 120);
	CHECK(disk_health(world, &d) && d == 15);

	/* 4. round trip, exact units; disk = Beta half-hearts (ceil, alive stays
	 * alive; 0 = quit-while-dead stays 0). */
	static const int16_t units[] = {160, 80, 8, 1, 77, 0};
	static const int16_t beta[] = {20, 10, 1, 1, 10, 0};
	for(size_t k = 0; k < sizeof units / sizeof units[0]; k++) {
		fresh_world(world);
		struct level_archive la;
		assert(level_archive_create(&la, world));
		CHECK(write_health(&la, units[k]));
		CHECK(level_archive_save(&la));
		level_archive_destroy(&la);
		CHECK(disk_health(world, &d) && d == beta[k]);
		CHECK(load_health(world, true) == units[k]);
		CHECK(load_health(world, false) == units[k]);
	}

	/* 5. external edit of Health (e.g. an NBT editor) beats a stale marker. */
	fresh_world(world);
	{
		struct level_archive la;
		assert(level_archive_create(&la, world));
		CHECK(write_health(&la, 77));
		set_raw_health(&la, 4); /* 2 hearts */
		CHECK(level_archive_save(&la));
		level_archive_destroy(&la);
	}
	CHECK(load_health(world, false) == 32);

	/* 6. guest records are CaveWii-only: a marker-less one is pre-fix units;
	 * one written by the new writer round-trips exactly. */
	fresh_world(world);
	{
		struct level_archive la;
		assert(level_archive_create(&la, world));
		remove_named(nbt_find_by_path(la.data, ".Data.Player"),
					 "CaveWiiHealth");
		set_raw_health(&la, 120);
		int16_t g = -1;
		CHECK(guest_read(&la, &g) && g == 120);
		CHECK(write_health(&la, 120));
		nbt_node* h = nbt_find_by_path(la.data, ".Data.Player.Health");
		CHECK(h && h->type == TAG_SHORT && h->payload.tag_short == 15);
		g = -1;
		CHECK(guest_read(&la, &g) && g == 120);
		level_archive_discard(&la);
	}

	string_clear(world);
	if(s_fail) {
		fprintf(stderr, "test_player_nbt_beta_types FAIL (%d checks)\n",
				s_fail);
		return 1;
	}
	puts("test_player_nbt_beta_types PASS");
	return 0;
}
