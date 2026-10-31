/* T09a/T09b: actual local mailbox/session lifecycle, migration byte identity,
 * backup preservation and the no-entry boundary.
 * Beta reference: 1.7.3-LTS server SaveConverterMcRegion.java:27-54,
 * converMapToMCRegion converts archives without a World/player. */
#include <assert.h>
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
#include "support/test_fixture_dir.h"
#include "game/client_session.h"
#include "network/level_archive.h"
#include "network/server_local.h"
#include "network/server_local_state.h"
#include "network/region_archive.h"
#include "network/server_world.h"
#include "network/tile_entities_nbt.h"
#include "network/world_copy.h"
#include "block/blocks.h"
#include "item/items.h"
#include "network/client_interface.h"

static unsigned rpc_count;
static void count_rpc(struct server_rpc* call, void* user) {
	(void)call; (void)user;
	rpc_count++;
}

static void file_copy(const char* from, const char* to) {
	FILE* a = fopen(from, "rb");
	FILE* b = fopen(to, "wb");
	assert(a && b);
	int c;
	while((c = fgetc(a)) != EOF)
		assert(fputc(c, b) != EOF);
	assert(!ferror(a));
	assert(fclose(a) == 0);
	assert(fclose(b) == 0);
}

/* Both directions catch unexpected extra output as well as changed bytes. */
static void tree_equal(const char* a, const char* b) {
	DIR* dir = opendir(a);
	assert(dir);
	struct dirent* entry;
	while((entry = readdir(dir))) {
		if(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		char ap[1024], bp[1024];
		snprintf(ap, sizeof ap, "%s/%s", a, entry->d_name);
		snprintf(bp, sizeof bp, "%s/%s", b, entry->d_name);
		if(test_fixture_is_dir(a, entry->d_name)) {
			tree_equal(ap, bp);
		} else {
			FILE* af = fopen(ap, "rb");
			FILE* bf = fopen(bp, "rb");
			assert(af && bf);
			int ac, bc;
			do {
				ac = fgetc(af); bc = fgetc(bf);
				assert(ac == bc);
			} while(ac != EOF);
			assert(!ferror(af) && !ferror(bf));
			assert(fclose(af) == 0);
			assert(fclose(bf) == 0);
		}
	}
	assert(closedir(dir) == 0);
}


static void tree_copy(const char* from, const char* to) {
	assert(MKDIR(to) == 0);
	DIR* dir = opendir(from);
	assert(dir);
	struct dirent* e;
	while((e = readdir(dir))) {
		if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		char a[1024], b[1024];
		int an = snprintf(a, sizeof a, "%s/%s", from, e->d_name);
		assert(an > 0 && (size_t)an < sizeof a);
		int bn = snprintf(b, sizeof b, "%s/%s", to, e->d_name);
		assert(bn > 0 && (size_t)bn < sizeof b);
		if(test_fixture_is_dir(from, e->d_name))
			tree_copy(a, b);
		else
			file_copy(a, b);
	}
	assert(closedir(dir) == 0);
}

static bool keep_health_tag(const nbt_node* n, void* unused) {
	(void)unused;
	return !n->name || strcmp(n->name, "CaveWiiHealth");
}

/* Same legacy block/TE witnesses as test_import_migrate_fast_equiv, in
 * both dimensions; each start version isolates a different chain entry. */
static void migration_fixture(const char* dir, int version, bool old_health,
	bool mixed) {
	assert(MKDIR(dir) == 0);
	string_t name;
	string_init_set_str(name, dir);
	struct level_archive level;
	assert(level_archive_create_new(&level, name, 173, (vec3){0.5F, 65, 0.5F}));
	nbt_node* v = nbt_find_by_path(level.data, LEVEL_CAVEWII_SAVE_VERSION.name);
	assert(v && v->type == TAG_INT);
	v->payload.tag_int = version;
	if(old_health) {
		level.data = nbt_filter_inplace(level.data, keep_health_tag, NULL);
		assert(level.data);
		nbt_node* h = nbt_find_by_path(level.data, LEVEL_PLAYER_HEALTH.name);
		assert(h && h->type == TAG_SHORT);
		h->payload.tag_short = 120; /* pre-fix CaveWii units, Beta health 15 */
	}
	assert(level_archive_save(&level));
	level_archive_discard(&level);
	char sub[1024];
	snprintf(sub, sizeof sub, "%s/region", dir);
	assert(MKDIR(sub) == 0);
	snprintf(sub, sizeof sub, "%s/DIM-1", dir);
	assert(MKDIR(sub) == 0);
	snprintf(sub, sizeof sub, "%s/DIM-1/region", dir);
	assert(MKDIR(sub) == 0);
	for(int d = 0; d < 2; d++) {
		const size_t count = CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT;
		struct server_chunk chunk = {0};
		chunk.ids = calloc(count, 1);
		chunk.metadata = calloc(count / 2, 1);
		chunk.lighting_sky = calloc(count / 2, 1);
		chunk.lighting_torch = calloc(count / 2, 1);
		chunk.heightmap = calloc(CHUNK_SIZE * CHUNK_SIZE, 1);
		assert(chunk.ids && chunk.metadata && chunk.lighting_sky
			&& chunk.lighting_torch && chunk.heightmap);
		chunk.populated = true;
		/* Even indices: the low metadata nibble is at index/2. */
		size_t sign = 64 + (3 + 3 * CHUNK_SIZE) * WORLD_HEIGHT;
		size_t chest = 64 + (2 + 2 * CHUNK_SIZE) * WORLD_HEIGHT;
		size_t spawn = 64 + (5 + 5 * CHUNK_SIZE) * WORLD_HEIGHT;
		size_t bed = 64 + (8 + 8 * CHUNK_SIZE) * WORLD_HEIGHT;
		/* test_bed_meta_migrate witness: legacy +X foot 4 -> Beta 3. */
		chunk.ids[bed] = BLOCK_BED;
		chunk.metadata[bed / 2] = version < 5 ? 4 : 3;
		chunk.ids[sign] = BLOCK_SIGN;
		chunk.metadata[sign / 2] = version < 2 ? 6 : 2;
		if(mixed) {
			chunk.ids[sign + WORLD_HEIGHT] = BLOCK_SIGN;
			chunk.metadata[(sign + WORLD_HEIGHT) / 2] = 2;
		}
		chunk.ids[chest] = version < 3 ? 95 : BLOCK_IRON_CHEST;
		chunk.ids[spawn] = BLOCK_MOB_SPAWNER;
		chunk.metadata[spawn / 2] = version < 4 ? 2 : 0;
		struct te_chest box = {0};
		box.x = 2; box.y = 64; box.z = 2;
		box.items[0] = (struct item_data){.id = version < 3 ? 95 : BLOCK_IRON_CHEST,
			.count = 1};
		struct te_spawner mob = {0};
		mob.x = 5; mob.y = 64; mob.z = 5;
		strcpy(mob.mob_id, "Skeleton"); mob.delay = 137;
		struct chunk_tile_entities te = {0};
		te.chests = &box; te.n_chest = 1;
		if(version >= 4) { te.spawners = &mob; te.n_spawner = 1; }
		struct region_archive region;
		assert(region_archive_create_new(&region, name, 0, 0,
			d ? WORLD_DIM_NETHER : WORLD_DIM_OVERWORLD));
		assert(region_archive_set_blocks(&region, 0, 0, &chunk, &te));
		assert(region_archive_sync(&region));
		assert(region_archive_destroy(&region));
		free(chunk.ids); free(chunk.metadata); free(chunk.lighting_sky);
		free(chunk.lighting_torch); free(chunk.heightmap);
	}
	string_clear(name);
}

/* This is exactly the archive phase ordinary Play runs. Dirty in-memory
 * health is intentionally discarded here, just as convert-only does. */
static bool reference_chain(const char* path, bool* dirty, int* version) {
	string_t name;
	string_init_set_str(name, path);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	bool ok = server_local_save_version_chain(&level, name, false, 0);
	*dirty = level.modified;
	*version = level_archive_get_save_version(&level);
	level_archive_discard(&level);
	string_clear(name);
	return ok;
}

static int64_t last_played(const char* dir) {
	string_t name;
	string_init_set_str(name, dir);
	struct level_archive level;
	assert(level_archive_create(&level, name));
	nbt_node* n = nbt_find_by_path(level.data, ".Data.LastPlayed");
	assert(n && n->type == TAG_LONG);
	int64_t value = n->payload.tag_long;
	level_archive_discard(&level);
	string_clear(name);
	return value;
}

static void no_entry(struct server_local* s, struct server_local* before,
	struct client_session* c, const char* dir, int64_t played) {
	assert(memcmp(s, before, sizeof *s) == 0);
	assert(!c->local_bound && !c->local_player);
	assert(last_played(dir) == played);
	char path[1024];
	snprintf(path, sizeof path, "%s/player.dat", dir);
	struct stat st;
	assert(stat(path, &st) != 0);
	/* No server messages of any kind were emitted (including LOAD_WORLD),
	 * so a remote provider could not have received one either. */
	svin_process_messages(count_rpc, NULL, false);
	assert(rpc_count == 0);
}

static void settle(struct client_session*, struct server_local*,
	enum svin_convert_outcome, bool);

static void compare_case(struct client_session* c, struct server_local* s,
	struct server_local* before, const char* root, const char* tag,
	int version, bool old_health, bool mixed, bool corrupt, bool imported) {
	char original[256], reference[256], pristine[256], backup_before[256];
	snprintf(original, sizeof original, "%s/%s", root, tag);
	snprintf(reference, sizeof reference, "%s/%s-ref", root, tag);
	snprintf(pristine, sizeof pristine, "%s/%s-before", root, tag);
	snprintf(backup_before, sizeof backup_before, "%s/%s-backup-before", root, tag);
	if(imported) {
		char fixture[1024];
		strcpy(fixture, __FILE__);
		char* sep = strrchr(fixture, '/');
		char* winsep = strrchr(fixture, '\\');
		if(winsep && (!sep || winsep > sep)) sep = winsep;
		assert(sep);
		strcpy(sep + 1, "fixtures/beta_import/world");
		tree_copy(fixture, original);
	} else {
		migration_fixture(original, version, old_health, mixed);
	}
	/* Real backup API, including its intentional LevelName suffix. Take a
	 * byte snapshot AFTER publication: conversion must not touch any byte. */
	struct world_copy* copy = world_copy_survey_begin(original, 4096);
	assert(copy);
	unsigned steps = 0;
	while(world_copy_step(copy, WORLD_COPY_STEP_MAX) == WORLD_COPY_SURVEYING)
		assert(++steps < 10000);
	assert(world_copy_get_result(copy)->state == WORLD_COPY_READY);
	assert(world_copy_start(copy, UINT64_MAX) == WORLD_COPY_COPYING);
	while(world_copy_step(copy, WORLD_COPY_STEP_MAX) == WORLD_COPY_COPYING)
		assert(++steps < 10000);
	assert(world_copy_get_result(copy)->state == WORLD_COPY_DONE);
	char backup[WORLD_COPY_PATH_MAX];
	strcpy(backup, world_copy_get_result(copy)->destination);
	assert(world_copy_destroy(copy) == WORLD_COPY_OK);
	tree_copy(backup, backup_before);
	tree_copy(original, pristine);
	if(corrupt) {
		char path[1024];
		snprintf(path, sizeof path, "%s/region/r.0.0.mcr", original);
		FILE* f = fopen(path, "r+b");
		assert(f && fseek(f, 8192 + 5, SEEK_SET) == 0);
		int byte = fgetc(f);
		assert(byte != EOF && fseek(f, -1, SEEK_CUR) == 0);
		assert(fputc(byte ^ 255, f) != EOF && fclose(f) == 0);
	}
	tree_copy(original, reference);
	int64_t played = last_played(original);
	/* Queued cancellation does no archive work, even on a corrupt world. */
	assert(client_session_convert_start(c, original));
	assert(client_session_convert_cancel(c));
	settle(c, s, SVIN_CONVERT_CANCELLED, false);
	tree_equal(original, reference); tree_equal(reference, original);
	no_entry(s, before, c, original, played);
	bool dirty;
	int final_version;
	bool ok = reference_chain(reference, &dirty, &final_version);
	assert(ok == !corrupt);
	assert(final_version == (corrupt || mixed ? version : CAVEWII_SAVE_VERSION_CURRENT));
	enum svin_convert_outcome expected = corrupt || mixed ? SVIN_CONVERT_FAILED
		: version == 5 && old_health ? SVIN_CONVERT_SAVE_PENDING : SVIN_CONVERT_SUCCEEDED;
	assert(client_session_convert_start(c, original));
	assert(!client_session_convert_poll(c));
	settle(c, s, expected, true);
	tree_equal(original, reference); tree_equal(reference, original);
	no_entry(s, before, c, original, played);
	tree_equal(backup, backup_before); tree_equal(backup_before, backup);
	/* The completed ticket cannot settle twice or settle the next retry. */
	uint64_t stale = c->convert.ticket;
	assert(!svin_convert_finish(stale, SVIN_CONVERT_SUCCEEDED, false));
	assert(!svin_convert_release(stale));
	if(corrupt) {
		char from[1024], to[1024];
		snprintf(from, sizeof from, "%s/region/r.0.0.mcr", pristine);
		snprintf(to, sizeof to, "%s/region/r.0.0.mcr", original);
		file_copy(from, to);
		snprintf(to, sizeof to, "%s/region/r.0.0.mcr", reference);
		file_copy(from, to);
		assert(reference_chain(reference, &dirty, &final_version));
		expected = SVIN_CONVERT_SUCCEEDED;
	}
	assert(client_session_convert_start(c, original));
	assert(!svin_convert_finish(stale, SVIN_CONVERT_FAILED, true));
	settle(c, s, expected, true);
	tree_equal(original, reference); tree_equal(reference, original);
	no_entry(s, before, c, original, played);
	tree_equal(backup, backup_before); tree_equal(backup_before, backup);
	if(old_health) {
		string_t name;
		string_init_set_str(name, original);
		struct level_archive level;
		assert(level_archive_create(&level, name));
		nbt_node* h = nbt_find_by_path(level.data, LEVEL_PLAYER_HEALTH.name);
		nbt_node* exact = nbt_find_by_path(level.data, ".Data.Player.CaveWiiHealth");
		assert(h && h->type == TAG_SHORT);
		if(version == CAVEWII_SAVE_VERSION_CURRENT) {
			assert(h->payload.tag_short == 120 && !exact);
			/* A pending-save receipt is honest: disk remains byte-identical. */
			tree_equal(original, pristine); tree_equal(pristine, original);
		} else {
			assert(h->payload.tag_short == 15);
			assert(exact && exact->type == TAG_SHORT && exact->payload.tag_short == 120);
		}
		level_archive_discard(&level);
		string_clear(name);
	}
	printf("convert_only: %s tree/backup/no-entry/retry PASS\n", tag);
}

static void settle(struct client_session* c, struct server_local* s,
	enum svin_convert_outcome expected, bool changed) {
	assert(c->convert_pending);
	uint32_t serial = c->convert_serial;
	server_local_convert_service(s);
	assert(client_session_convert_poll(c));
	assert(!c->convert_pending && c->convert.outcome == expected);
	assert(c->convert.may_have_changed == changed);
	assert(c->convert_serial == serial + 1);
	assert(!client_session_convert_poll(c));
	assert(c->convert_serial == serial + 1);
	assert(!c->local_player && !c->local_bound);
}


static unsigned world_resets, load_failures;
static void drain_client(const struct client_rpc* call, void* unused) {
	(void)unused;
	world_resets += call->type == CRPC_WORLD_RESET;
	load_failures += call->type == CRPC_LOAD_FAILED;
	clin_drain_discard();
}

static void play(struct server_local* s, const char* path) {
	struct server_rpc call = {.type = SRPC_LOAD_WORLD};
	string_init_set_str(call.payload.load_world.name, path);
	assert(svin_rpc_send(&call));
	server_local_test_process_messages(s);
	assert(s->player.has_pos && !s->load_failure_terminal);
	assert(server_local_world_slot_state(s, 0) == SERVER_WORLD_SLOT_LIVE);
	clin_drain_discard();
}

static void update_and_play(struct client_session* c, struct server_local* s,
	const char* root) {
	char converted[256], ordinary[256];
	snprintf(converted, sizeof converted, "%s/update-convert", root);
	snprintf(ordinary, sizeof ordinary, "%s/ordinary-play", root);
	migration_fixture(converted, 3, true, false);
	tree_copy(converted, ordinary);
	blocks_init(); items_init();
	assert(clin_init());
	clin_test_set_rpc_observer(drain_client, NULL);
	server_local_test_create_without_thread(s, false, false, false, false,
		false, true, false);
	svin_set_local_motion(true);
	int64_t played = last_played(converted);
	assert(client_session_convert_start(c, converted));
	/* Production update dispatch, no manual mailbox take/finish. */
	server_local_test_update_once(s);
	/* Cancellation after the real chain has published cannot retract it. */
	assert(!client_session_convert_cancel(c));
	assert(client_session_convert_poll(c));
	assert(c->convert.outcome == SVIN_CONVERT_SUCCEEDED);
	assert(c->convert.may_have_changed && !c->convert_pending);
	assert(!client_session_convert_poll(c));
	assert(!s->player.has_pos && !s->guest_actor);
	for(unsigned i = 0; i < SERVER_LOCAL_WORLD_SLOTS; i++) {
		assert(s->world_slot_state[i] == SERVER_WORLD_SLOT_EMPTY);
		assert(s->worlds[i].generation == 0);
	}
	assert(last_played(converted) == played);
	assert(world_resets == 0 && load_failures == 0);
	svin_process_messages(count_rpc, NULL, false);
	assert(rpc_count == 0);

	/* Ordinary Play still migrates and enters an old world in one dispatch.
	 * Its archive result must equal the convert-only tree, byte for byte. */
	play(s, ordinary);
	assert(world_resets == 1 && load_failures == 0);
	assert(s->player.health == 120);
	double x = s->player.x, y = s->player.y, z = s->player.z;
	tree_equal(converted, ordinary); tree_equal(ordinary, converted);
	assert(server_local_unload_world_impl(s, false));
	clin_drain_discard();
	/* Only this explicit Play enters the already-converted original. */
	play(s, converted);
	assert(world_resets == 2 && load_failures == 0);
	assert(s->player.health == 120);
	assert(s->player.x == x && s->player.y == y && s->player.z == z);
	assert(server_local_unload_world_impl(s, false));
	clin_drain_discard();

	/* A failed ordinary Play is still an orderly terminal loading failure,
	 * unlike convert-only failure, which remains retryable in the menu. */
	struct server_rpc fail = {.type = SRPC_LOAD_WORLD};
	string_init_printf(fail.payload.load_world.name, "%s/not-a-world", root);
	assert(svin_rpc_send(&fail));
	server_local_test_process_messages(s);
	clin_drain_discard();
	assert(!s->player.has_pos && s->load_failure_terminal);
	assert(load_failures == 1);
	clin_test_set_rpc_observer(NULL, NULL);
	server_local_shutdown(s);
	svin_set_local_motion(false);
	puts("convert_only: production update + ordinary Play dispatch PASS");
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	assert(svin_init());
	region_archive_test_clock(1700000000u);
	struct client_session* c = calloc(1, sizeof *c);
	struct server_local* s = calloc(1, sizeof *s);
	struct server_local* before = calloc(1, sizeof *s);
	assert(c && s && before);
	char root[160], converted[256], reference[256], ap[512], bp[512];
	test_fixture_dir("test_convert_only_tmp", root, sizeof root);
	assert(MKDIR(root) == 0);
	snprintf(converted, sizeof converted, "%s/converted", root);
	snprintf(reference, sizeof reference, "%s/reference", root);
	assert(MKDIR(converted) == 0 && MKDIR(reference) == 0);
	string_t name;
	string_init_set_str(name, converted);
	struct level_archive level;
	assert(level_archive_create_new(&level, name, 173, (vec3){0.5F, 65, 0.5F}));
	nbt_node* v = nbt_find_by_path(level.data, LEVEL_CAVEWII_SAVE_VERSION.name);
	assert(v && v->type == TAG_INT);
	v->payload.tag_int = CAVEWII_SAVE_VERSION_MOB_SPAWNER_TES;
	assert(level_archive_save(&level));
	level_archive_discard(&level);
	snprintf(ap, sizeof ap, "%s/level.dat", converted);
	snprintf(bp, sizeof bp, "%s/level.dat", reference);
	file_copy(ap, bp);

	/* Remote/offline admission emits no command and preserves the receipt. */
	assert(!client_session_convert_start(c, converted));
	svin_set_local_motion(true);
	assert(!client_session_convert_start(c, ""));
	char long_path[SVIN_CONVERT_PATH_MAX + 1];
	memset(long_path, 'x', sizeof long_path - 1);
	long_path[sizeof long_path - 1] = 0;
	assert(!client_session_convert_start(c, long_path));

	/* Cancel before execution preserves every file and never enters a world. */
	assert(client_session_convert_start(c, converted));
	uint64_t first = c->convert.ticket;
	assert(!client_session_convert_start(c, reference));
	assert(!svin_convert_request(reference));
	assert(!svin_convert_cancel(first + 1));
	assert(client_session_convert_cancel(c));
	assert(client_session_convert_cancel(c));
	settle(c, s, SVIN_CONVERT_CANCELLED, false);
	tree_equal(converted, reference); tree_equal(reference, converted);

	/* The reference invokes exactly today's load-time migration chain. */
	string_set_str(name, reference);
	assert(level_archive_create(&level, name));
	assert(server_local_save_version_chain(&level, name, false, 0));
	level_archive_discard(&level);
	assert(client_session_convert_start(c, converted));
	assert(c->convert.ticket > first);
	assert(!svin_convert_finish(first, SVIN_CONVERT_SUCCEEDED, true));
	settle(c, s, SVIN_CONVERT_SUCCEEDED, true);
	tree_equal(converted, reference); tree_equal(reference, converted);
	assert(memcmp(s, before, sizeof *s) == 0);
	assert(!svin_convert_finish(c->convert.ticket, SVIN_CONVERT_FAILED, true));

	compare_case(c, s, before, root, "wall-sign", 0, false, false, false, false);
	compare_case(c, s, before, root, "iron-chest", 2, false, false, false, false);
	compare_case(c, s, before, root, "spawner", 3, false, false, false, false);
	compare_case(c, s, before, root, "health", 4, true, false, false, false);
	compare_case(c, s, before, root, "v5-health", 5, true, false, false, false);
	compare_case(c, s, before, root, "beta-import", 0, false, false, false, true);
	compare_case(c, s, before, root, "mixed-refusal", 0, false, true, false, false);
	compare_case(c, s, before, root, "corrupt-repair", 0, false, false, true, false);

	/* A real archive-open failure settles once and allows a retry. */
	snprintf(ap, sizeof ap, "%s/missing", root);
	assert(client_session_convert_start(c, ap));
	settle(c, s, SVIN_CONVERT_OPEN_FAILED, false);
	assert(client_session_convert_start(c, converted));
	settle(c, s, SVIN_CONVERT_SUCCEEDED, true);
	s->player.has_pos = true;
	assert(client_session_convert_start(c, converted));
	settle(c, s, SVIN_CONVERT_WORLD_ACTIVE, false);
	s->player.has_pos = false;
	s->world_slot_state[1] = SERVER_WORLD_SLOT_RETIRING;
	assert(client_session_convert_start(c, converted));
	settle(c, s, SVIN_CONVERT_WORLD_ACTIVE, false);
	s->world_slot_state[1] = SERVER_WORLD_SLOT_EMPTY;

	/* Running cancellation waits for a safe chain boundary and truthfully
	 * preserves the possibility of disk changes. Stale/duplicate completions
	 * cannot finish another ticket or advance the owning session twice. */
	assert(client_session_convert_start(c, converted));
	uint64_t ticket;
	const char* path;
	bool cancelled;
	assert(svin_convert_take(&ticket, &path, &cancelled));
	assert(!cancelled && !strcmp(path, converted));
	assert(!svin_convert_finish(first, SVIN_CONVERT_SUCCEEDED, true));
	assert(!svin_convert_take(&ticket, &path, &cancelled));
	bool dirty;
	int version;
	assert(reference_chain(path, &dirty, &version));
	assert(version == CAVEWII_SAVE_VERSION_CURRENT && !dirty);
	assert(client_session_convert_cancel(c));
	assert(!client_session_convert_poll(c));
	assert(svin_convert_finish(ticket, SVIN_CONVERT_SUCCEEDED, true));
	assert(!svin_convert_finish(ticket, SVIN_CONVERT_SUCCEEDED, true));
	assert(client_session_convert_poll(c));
	assert(c->convert.outcome == SVIN_CONVERT_CANCELLED && c->convert.may_have_changed);

	/* Provider teardown settles a queued command, without sending remotely. */
	assert(client_session_convert_start(c, converted));
	svin_set_local_motion(false);
	assert(client_session_convert_poll(c));
	assert(c->convert.outcome == SVIN_CONVERT_PROVIDER_STOPPED);
	assert(!client_session_convert_start(c, converted));
	svin_process_messages(count_rpc, NULL, false);
	assert(rpc_count == 0);
	assert(memcmp(s, before, sizeof *s) == 0);
	update_and_play(c, s, root);
	region_archive_test_clock(0);
	string_clear(name);
	free(before); free(s); free(c);
	test_fixture_dir_cleanup(root);
	puts("convert_only: PASS (lifecycle, byte equality, no entry, no remote RPC)");
	return 0;
}
