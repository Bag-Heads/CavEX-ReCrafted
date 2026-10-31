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

#include <assert.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
// devkitPPC's newlib hides these POSIX declarations under -std=c99 -pedantic.
int fileno(FILE* f);
int fsync(int fd);
#endif

#include "level_archive.h"
#ifdef PLATFORM_WII
#include "save_path.h"
#endif
#include "../item/item_nbt.h"
#include "level_template.h"
#include "server_interface.h"
#include "region_archive.h"
#ifdef PLATFORM_WII
#include "server_local.h"
#include "../platform/time.h"
/* `diag` is false when the flush runs on the deferred IO worker (G9 R19):
 * the stage word describes the SERVER thread. */
#define SAVE_DIAG(stage) do { if(diag) server_diag_set(stage); } while(0)
#else
#define SAVE_DIAG(stage) ((void)0)
#endif
#ifdef CAVEWII_REGION_ARCHIVE_TEST
#define LEVEL_TRACE(op, path) region_archive_test_trace_note((op), (path), 0, 0)
#else
#define LEVEL_TRACE(op, path) ((void)0)
#endif

#define LEVEL_PLAYER_XZ_LIMIT 30000000.0f
#define LEVEL_PLAYER_Y_LIMIT 4096.0f
#define LEVEL_PLAYER_VELOCITY_LIMIT 10000.0f

static bool level_archive_version_supported(nbt_node* root) {
	nbt_node* node = nbt_find_by_path(root,
								 LEVEL_CAVEWII_SAVE_VERSION.name);
	if(!node)
		return true;
	return node->type == TAG_INT
		&& node->payload.tag_int >= CAVEWII_SAVE_VERSION_LEGACY
		&& node->payload.tag_int <= CAVEWII_SAVE_VERSION_CURRENT;
}

static bool level_archive_accept_parsed(struct level_archive* la) {
	if(!la->data)
		return false;
	if(level_archive_version_supported(la->data)) {
		la->lossless_blocked = !item_nbt_scope_writable(la->data);
		return true;
	}

	nbt_node* version = nbt_find_by_path(la->data, LEVEL_CAVEWII_SAVE_VERSION.name);
	// A valid future version is refused, never corruption eligible for recovery.
	// Malformed/negative versions are content failures; this parsed-tree path
	// still fails closed without promoting a backup, as before.
	int status = version && version->type == TAG_INT
		&& version->payload.tag_int > CAVEWII_SAVE_VERSION_CURRENT
		? NBT_EREFUSED : NBT_ERR;
	nbt_free(la->data);
	la->data = NULL;
	errno = status;
	return false;
}

static bool level_archive_create_impl(struct level_archive* la, string_t filename) {
	assert(la && filename);
#ifdef PLATFORM_WII
	save_path_set_world(string_get_cstr(filename));
#endif

	la->modified = false;
	la->lossless_blocked = false;
	la->inventory_write_refused = false;
	la->pending_job = NULL;
	la->save_batch_depth = 0;
	la->save_batch_pending = false;
	/* G9 R19: another archive's queued background save of this file must
	 * land before it is read (no-op while deferred IO is off). */
	region_archive_io_barrier_all();

	string_init_printf(la->file_name, "%s/level.dat",
					   string_get_cstr(filename));

	la->data = nbt_parse_path(string_get_cstr(la->file_name));
	if(!la->data && nbt_classify_load_failure(errno) == NBT_OPERATIONAL) {
		int status = errno;
		string_clear(la->file_name);
		errno = status;
		return false;
	}
	if(la->data) {
		if(level_archive_accept_parsed(la))
			return true;
		// A valid tree from a newer or malformed schema must not silently fall
		// back to an older backup and roll the world backward.
		string_clear(la->file_name);
		return false;
	}

	// Storage assumption: completed renames leave readable ordered survivors;
	// interrupted directory/FAT damage is outside this file-level model.
	// libdvm's _FAT_rename_r delegates to f_rename (the libfat compatibility path):
	// https://github.com/devkitPro/libdvm/blob/master/source/fat_driver.c
	// Two renames are NOT an atomic rotation. Validation before rename remains
	// mandatory; operational read failures never authorize replacing a primary.
	// RECOVERY (2026-06-12 review): the safe-rotation write in
	// level_archive_flush leaves level.dat absent for the window between its
	// two renames -- a power cut there used to make the world permanently
	// unopenable despite an intact backup. Fall back to the survivors, newest
	// first: level.dat_new (a complete write that never got renamed; a partial
	// write fails nbt_parse and is skipped), then level.dat_old (the previous
	// good copy). nbt_parse_path self-validates (gzip + tree), so a parse
	// success means the file is whole. Restore the survivor into place.
	const char* suffix[] = {"_new", "_old"};
	for(size_t k = 0; k < sizeof(suffix) / sizeof(*suffix); k++) {
		string_t cand;
		string_init_printf(cand, "%s%s", string_get_cstr(la->file_name),
						   suffix[k]);
		la->data = nbt_parse_path(string_get_cstr(cand));
		if((!la->data && nbt_classify_load_failure(errno) == NBT_OPERATIONAL)
		   || (la->data && !level_archive_accept_parsed(la))) {
			string_clear(cand);
			string_clear(la->file_name);
			return false;
		}
		if(la->data) {
			// Primary is confirmed absent/corrupt, candidate already validated.
			// FAT/Windows rename need not replace an existing destination.
			if((remove(string_get_cstr(la->file_name)) != 0 && errno != ENOENT)
			   || rename(string_get_cstr(cand), string_get_cstr(la->file_name)) != 0) {
				nbt_free(la->data); la->data = NULL;
				string_clear(cand); string_clear(la->file_name);
				errno = NBT_EIO;
				return false;
			}
			string_clear(cand);
			return true;
		}
		string_clear(cand);
	}

	string_clear(la->file_name);
	return false;
}

bool level_archive_create(struct level_archive* la, string_t filename) {
#ifdef PLATFORM_WII
	bool profile = region_archive_work_profile_enabled();
	if(profile) region_archive_path_profile_emit("load_begin", 0);
	ptime_t start = profile ? time_get() : 0;
#endif
	bool ok = level_archive_create_impl(la, filename);
#ifdef PLATFORM_WII
	int saved_errno = errno;
	if(profile) {
		uint32_t elapsed_us = (uint32_t)(time_diff_s(start, time_get()) * 1e6f);
		region_archive_path_profile_note(elapsed_us);
		region_archive_path_profile_emit("level_load_end", elapsed_us);
	}
	errno = saved_errno;
#endif
	return ok;
}

bool level_archive_create_new(struct level_archive* la, string_t directory,
							  int64_t seed, vec3 spawn) {
	assert(la && directory && spawn);
#ifdef PLATFORM_WII
	save_path_set_world(string_get_cstr(directory));
#endif

	// Start from the bundled minimal Beta level.dat template (tools/
	// gen_level_template.py) rather than hand-building the cNBT tree, then
	// overwrite the dynamic fields through the proven write path.
	la->pending_job = NULL;
	la->modified = false;
	la->lossless_blocked = false;
	la->inventory_write_refused = false;
	la->save_batch_depth = 0;
	la->save_batch_pending = false;
	string_init_printf(la->file_name, "%s/level.dat",
					   string_get_cstr(directory));
	la->data = nbt_parse_compressed(LEVEL_DAT_TEMPLATE, sizeof(LEVEL_DAT_TEMPLATE));
	if(!la->data)
		return false;

	int32_t sx = (int32_t)floorf(spawn[0]);
	int32_t sy = (int32_t)floorf(spawn[1]);
	int32_t sz = (int32_t)floorf(spawn[2]);

	// Generator pin (P-1): stamp B173 explicitly even though the template
	// already carries it -- level_archive_write cannot CREATE nodes, so this
	// write failing loudly means the embedded template is stale (regenerate
	// via tools/gen_level_template.py).
	int32_t generator = CAVEWII_GENERATOR_B173;
	int32_t save_version = CAVEWII_SAVE_VERSION_CURRENT;
	int16_t full_health = MAX_PLAYER_HEALTH;

	if(!level_archive_write(la, LEVEL_RANDOM_SEED, &seed)
	   || !level_archive_write(la, LEVEL_CAVEWII_GENERATOR, &generator)
	   || !level_archive_write(la, LEVEL_CAVEWII_SAVE_VERSION, &save_version)
	   || !level_archive_write_player_health(la, full_health)
	   || !level_archive_write(la, LEVEL_WORLD_SPAWNX, &sx)
	   || !level_archive_write(la, LEVEL_WORLD_SPAWNY, &sy)
	   || !level_archive_write(la, LEVEL_WORLD_SPAWNZ, &sz)
	   || !level_archive_write_player(la, spawn, (vec2) {0.0F, 0.0F},
									  (vec3) {0.0F, 0.0F, 0.0F},
									  WORLD_DIM_OVERWORLD)) {
		nbt_free(la->data);
		la->data = NULL;
		string_clear(la->file_name);
		return false;
	}

	la->modified = true; // ensures level_archive_destroy writes it out
	return true;
}

static bool level_archive_read_internal(nbt_node* root,
										struct level_archive_tag tag,
										void* result, size_t length) {
	assert(root && result);
	assert((tag.type == TAG_STRING && length > 0)
		   || (tag.type != TAG_STRING && length == 0));

	nbt_node* node = nbt_find_by_path(root, tag.name);

	if(!node || node->type != tag.type)
		return false;

	switch(tag.type) {
		case TAG_COMPOUND:
		case TAG_LIST: *((nbt_node**)result) = node; return true;
		case TAG_STRING:
			if(length > 0) {
				strncpy(result, node->payload.tag_string, length - 1);
				((char*)result)[length - 1] = 0;
			}
			return true;
		case TAG_LONG:
			*((int64_t*)result) = node->payload.tag_long;
			return true;
		case TAG_INT: *((int32_t*)result) = node->payload.tag_int; return true;
		case TAG_SHORT:
			*((int16_t*)result) = node->payload.tag_short;
			return true;
		case TAG_BYTE: *((int8_t*)result) = node->payload.tag_byte; return true;
		case TAG_FLOAT: *((float*)result) = node->payload.tag_float; return true;
		case TAG_DOUBLE: *((double*)result) = node->payload.tag_double; return true;
		default: return false;
	}
}

bool level_archive_read(struct level_archive* la, struct level_archive_tag tag,
						void* result, size_t length) {
	assert(la && result);
	assert(la->data);
	return level_archive_read_internal(la->data, tag, result, length);
}

int32_t level_archive_get_save_version(const struct level_archive* la) {
	assert(la && la->data);

	nbt_node* node = nbt_find_by_path(la->data,
								 LEVEL_CAVEWII_SAVE_VERSION.name);
	return node ? node->payload.tag_int : CAVEWII_SAVE_VERSION_LEGACY;
}

bool level_archive_was_created_by_cavewii(const struct level_archive* la) {
	assert(la && la->data);

	nbt_node* node
		= nbt_find_by_path(la->data, LEVEL_CAVEWII_GENERATOR.name);
	return node && node->type == TAG_INT
		&& node->payload.tag_int == CAVEWII_GENERATOR_B173;
}

bool level_archive_has_wall_sign_legacy_intent(
	const struct level_archive* la) {
	assert(la && la->data);
	nbt_node* node = nbt_find_by_path(
		la->data, LEVEL_CAVEWII_WALL_SIGN_LEGACY_INTENT.name);
	return node && node->type == TAG_BYTE && node->payload.tag_byte == 1;
}

bool level_archive_record_wall_sign_legacy_intent(struct level_archive* la) {
	assert(la && la->data);
	nbt_node* node = nbt_find_by_path(
		la->data, LEVEL_CAVEWII_WALL_SIGN_LEGACY_INTENT.name);
	if(node)
		return node->type == TAG_BYTE && node->payload.tag_byte == 1;

	nbt_node* data = nbt_find_by_path(la->data, ".Data");
	if(!data || data->type != TAG_COMPOUND || !data->payload.tag_compound)
		return false;

	node = calloc(1, sizeof(*node));
	struct nbt_list* entry = malloc(sizeof(*entry));
	const char name[] = "CaveWiiWallSignLegacyIntent";
	char* node_name = malloc(sizeof(name));
	if(!node || !entry || !node_name) {
		free(node);
		free(entry);
		free(node_name);
		return false;
	}

	memcpy(node_name, name, sizeof(name));
	node->type = TAG_BYTE;
	node->name = node_name;
	node->payload.tag_byte = 1;
	entry->data = node;
	list_add_tail(&entry->entry, &data->payload.tag_compound->entry);
	la->modified = true;
	return level_archive_save(la);
}

static bool level_archive_stamp_save_version(struct level_archive* la,
											 int32_t version) {
	nbt_node* node = nbt_find_by_path(la->data,
								 LEVEL_CAVEWII_SAVE_VERSION.name);
	if(node) {
		if(node->type != TAG_INT)
			return false;
		node->payload.tag_int = version;
		return true;
	}

	nbt_node* data = nbt_find_by_path(la->data, ".Data");
	if(!data || data->type != TAG_COMPOUND || !data->payload.tag_compound)
		return false;

	node = calloc(1, sizeof(*node));
	struct nbt_list* entry = malloc(sizeof(*entry));
	const char name[] = "CaveWiiSaveVersion";
	char* node_name = malloc(sizeof(name));
	if(!node || !entry || !node_name) {
		free(node);
		free(entry);
		free(node_name);
		return false;
	}

	memcpy(node_name, name, sizeof(name));
	node->type = TAG_INT;
	node->name = node_name;
	node->payload.tag_int = version;
	entry->data = node;
	list_add_tail(&entry->entry, &data->payload.tag_compound->entry);
	return true;
}

bool level_archive_upgrade_save_version(struct level_archive* la,
										int32_t version) {
	assert(la && la->data);
	assert(version > CAVEWII_SAVE_VERSION_LEGACY
		   && version <= CAVEWII_SAVE_VERSION_CURRENT);
	if(level_archive_get_save_version(la) >= version)
		return true;
	if(!level_archive_stamp_save_version(la, version))
		return false;
	la->modified = true;
	if(la->save_batch_depth > 0) {
		/* dropin-1029: one level.dat write for the whole save-version chain,
		 * so the first rotation moves the world's own (e.g. Beta-written)
		 * level.dat to level.dat_old and no later stamp overwrites it. */
		la->save_batch_pending = true;
		return true;
	}
	return level_archive_save(la);
}

void level_archive_save_batch_begin(struct level_archive* la) {
	assert(la && la->save_batch_depth < UINT8_MAX);
	la->save_batch_depth++;
}

bool level_archive_save_batch_flush(struct level_archive* la) {
	assert(la && la->save_batch_depth > 0);
	if(!la->save_batch_pending)
		return true;
	la->save_batch_pending = false;
	return level_archive_save(la);
}

#ifdef CAVEWII_SERVER_LOCAL_TEST
void level_archive_save_batch_abandon(struct level_archive* la) {
	la->save_batch_depth = 0;
	la->save_batch_pending = false;
}
#endif

bool level_archive_save_batch_end(struct level_archive* la) {
	assert(la && la->save_batch_depth > 0);
	if(--la->save_batch_depth > 0 || !la->save_batch_pending)
		return true;
	la->save_batch_pending = false;
	return level_archive_save(la);
}

/* ---- dropin-1029: world origin (see level_archive.h) ---------------------- */
bool level_archive_player_beta_born(const struct level_archive* la) {
	assert(la && la->data);
	nbt_node* player = nbt_find_by_path(la->data, ".Data.Player");
	if(!player || player->type != TAG_COMPOUND || !player->payload.tag_compound)
		return false;
	struct list_head* cur;
	list_for_each(cur, &player->payload.tag_compound->entry) {
		nbt_node* n = list_entry(cur, struct nbt_list, entry)->data;
		if(n && n->name
		   && (!strcmp(n->name, "Score") || !strcmp(n->name, "Sleeping")
			   || !strcmp(n->name, "SleepTimer")))
			return true;
	}
	return false;
}

struct level_origin_probe {
	unsigned chunks;
	bool cavewii_marker;   /* a chunk with a .Level.CaveWii* tag */
};

static void level_origin_probe_chunk(nbt_node* chunk,
									 struct level_origin_probe* p) {
	nbt_node* level = nbt_find_by_path(chunk, ".Level");
	if(!level || level->type != TAG_COMPOUND || !level->payload.tag_compound)
		return;
	struct list_head* cur;
	list_for_each(cur, &level->payload.tag_compound->entry) {
		nbt_node* n = list_entry(cur, struct nbt_list, entry)->data;
		if(!n || !n->name)
			continue;
		if(!strncmp(n->name, "CaveWii", 7))
			p->cavewii_marker = true;
	}
}

/* Read-only: raw McRegion framing (4 KiB offset table, big-endian
 * offset<<8|sectors; record = u32 BE length, u8 compression, payload). */
static void level_origin_probe_region(const char* path,
									  struct level_origin_probe* p) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return;
	uint8_t header[4096];
	if(fread(header, 1, sizeof header, f) != sizeof header) {
		fclose(f);
		return;
	}
	for(int i = 0; i < 1024 && p->chunks < LEVEL_ORIGIN_PROBE_CHUNKS; i++) {
		uint32_t off = ((uint32_t)header[i * 4] << 16)
			| ((uint32_t)header[i * 4 + 1] << 8) | header[i * 4 + 2];
		uint8_t sectors = header[i * 4 + 3];
		if(!off || !sectors || off < 2)
			continue;
		uint8_t frame[5];
		if(fseek(f, (long)off * 4096L, SEEK_SET) != 0
		   || fread(frame, 1, 5, f) != 5)
			continue;
		uint32_t len = ((uint32_t)frame[0] << 24) | ((uint32_t)frame[1] << 16)
			| ((uint32_t)frame[2] << 8) | frame[3];
		if(len < 2 || len > (uint32_t)sectors * 4096u || (frame[4] != 1 && frame[4] != 2))
			continue;
		void* data = malloc(len - 1);
		if(!data)
			break;
		if(fread(data, 1, len - 1, f) == len - 1) {
			nbt_node* chunk = nbt_parse_compressed(data, len - 1);
			if(chunk) {
				p->chunks++;
				level_origin_probe_chunk(chunk, p);
				nbt_free(chunk);
			}
		}
		free(data);
	}
	fclose(f);
}

/* dropin-1029 review M2: the world list resolves every row on each rebuild;
 * the probe costs ~0.1-0.2 s per world on the Wii. Cache the answer per
 * level.dat path, keyed by its size + mtime (any save of an unstamped world
 * changes level.dat, so a stale entry cannot survive a write by this port). */
#define LEVEL_ORIGIN_CACHE 16
static struct {
	char path[256];
	long long size, mtime;
	enum level_world_origin origin;
	bool used;
} s_origin_cache[LEVEL_ORIGIN_CACHE];
static unsigned s_origin_cache_next;

enum level_world_origin level_archive_world_origin(const struct level_archive* la) {
	assert(la && la->data);
	if(level_archive_was_created_by_cavewii(la))
		return LEVEL_ORIGIN_CAVEWII;

	const char* lpath = string_get_cstr(la->file_name);
	struct stat st;
	bool keyed = stat(lpath, &st) == 0 && strlen(lpath) < sizeof(s_origin_cache[0].path);
	if(keyed)
		for(unsigned i = 0; i < LEVEL_ORIGIN_CACHE; i++)
			if(s_origin_cache[i].used && s_origin_cache[i].size == (long long)st.st_size
			   && s_origin_cache[i].mtime == (long long)st.st_mtime
			   && !strcmp(s_origin_cache[i].path, lpath))
				return s_origin_cache[i].origin;

	/* Review M1: ANY CaveWii-made evidence wins over Beta evidence, so a Beta
	 * world once played in a pre-06-11 CaveWii still gets the backup question
	 * before the chain may rewrite it. Positive evidence only: a CaveWii* chunk
	 * tag, or a Player born in a CaveWii template (no Score/Sleeping/
	 * SleepTimer). LastUpdate == 0 is NOT evidence: a real JAR-made Beta world
	 * (tests/fixtures/beta_import/world) has it on all 32 chunks -- chunks
	 * saved at world time 0 and never modified keep it. */
	nbt_node* player = nbt_find_by_path(la->data, ".Data.Player");
	bool template_player = player && player->type == TAG_COMPOUND
		&& !level_archive_player_beta_born(la);

	struct level_origin_probe probe = {0};
	if(!template_player) {
		string_t dir;
		string_init_set(dir, la->file_name);
		size_t cut = string_search_rchar(dir, '/');
		if(cut != STRING_FAILURE) {
			string_left(dir, cut);
			string_cat_str(dir, "/region");
			DIR* d = opendir(string_get_cstr(dir));
			if(d) {
				struct dirent* e;
				unsigned files = 0;
				while(files < 2 && probe.chunks < LEVEL_ORIGIN_PROBE_CHUNKS
					  && !probe.cavewii_marker && (e = readdir(d))) {
					size_t n = strlen(e->d_name);
					if(n < 5 || strcmp(e->d_name + n - 4, ".mcr"))
						continue;
					string_t path;
					string_init_printf(path, "%s/%s", string_get_cstr(dir),
									   e->d_name);
					level_origin_probe_region(string_get_cstr(path), &probe);
					string_clear(path);
					files++;
				}
				closedir(d);
			}
		}
		string_clear(dir);
	}

	enum level_world_origin origin = (template_player || probe.cavewii_marker)
		? LEVEL_ORIGIN_CAVEWII_LEGACY
		: LEVEL_ORIGIN_BETA;
	if(keyed) {
		unsigned slot = s_origin_cache_next++ % LEVEL_ORIGIN_CACHE;
		snprintf(s_origin_cache[slot].path, sizeof(s_origin_cache[slot].path),
				 "%s", lpath);
		s_origin_cache[slot].size = (long long)st.st_size;
		s_origin_cache[slot].mtime = (long long)st.st_mtime;
		s_origin_cache[slot].origin = origin;
		s_origin_cache[slot].used = true;
	}
	return origin;
}


static bool level_archive_write_internal(nbt_node* root,
										 struct level_archive_tag tag,
										 void* data) {
	assert(root && data);

	nbt_node* node = nbt_find_by_path(root, tag.name);

	if(!node || node->type != tag.type)
		return false;

	switch(tag.type) {
		case TAG_LONG: node->payload.tag_long = *(int64_t*)data; return true;
		case TAG_INT: node->payload.tag_int = *(int32_t*)data; return true;
		case TAG_SHORT: node->payload.tag_short = *(int16_t*)data; return true;
		case TAG_BYTE: node->payload.tag_byte = *(int8_t*)data; return true;
		case TAG_FLOAT: node->payload.tag_float = *(float*)data; return true;
		case TAG_DOUBLE: node->payload.tag_double = *(double*)data; return true;
		case TAG_STRING: {
			/* `data` is a NUL-terminated char*. cNBT owns tag_string, so
			 * swap in a fresh copy -- allocate FIRST so an OOM leaves the
			 * old value intact (report false, nothing torn). */
			const char* src = data;
			size_t n = strlen(src) + 1;
			char* copy = malloc(n);
			if(!copy)
				return false;
			memcpy(copy, src, n);
			free(node->payload.tag_string);
			node->payload.tag_string = copy;
			return true;
		}
		default: return false;
	}
}

bool level_archive_write(struct level_archive* la, struct level_archive_tag tag,
						 void* data) {
	assert(la && data);
	assert(la->data);

	if(level_archive_write_internal(la->data, tag, data)) {
		la->modified = true;
		return true;
	}

	return false;
}

bool level_archive_is_creative(const struct level_archive* la) {
	assert(la && la->data);
	nbt_node* node = nbt_find_by_path(la->data, LEVEL_GAME_MODE.name);
	/* Absent (every pre-2026-09-01 world), wrong-type, or an unknown value:
	 * SURVIVAL. Fails closed -- corruption can never grant creative. */
	if(!node || node->type != TAG_BYTE)
		return false;
	return node->payload.tag_byte == LEVEL_GAME_MODE_CREATIVE;
}

bool level_archive_write_game_mode(struct level_archive* la, bool creative) {
	assert(la && la->data);

	int8_t value
		= creative ? LEVEL_GAME_MODE_CREATIVE : LEVEL_GAME_MODE_SURVIVAL;

	nbt_node* node = nbt_find_by_path(la->data, LEVEL_GAME_MODE.name);
	if(node) {
		/* CaveWii owns this tag and the reader already discards a malformed
		 * payload as survival -- coerce a wrong-type node rather than wedging
		 * every future write against it (the DeathTxCounter precedent). */
		node->type = TAG_BYTE;
		node->payload.tag_byte = value;
		la->modified = true;
		return true;
	}

	/* Legacy/imported worlds predate the tag. Allocate the whole list entry
	 * before linking it so OOM leaves the live archive byte-for-byte
	 * unchanged (the FallDistance migration pattern). */
	nbt_node* data = nbt_find_by_path(la->data, ".Data");
	if(!data || data->type != TAG_COMPOUND || !data->payload.tag_compound)
		return false;

	node = calloc(1, sizeof(*node));
	struct nbt_list* entry = malloc(sizeof(*entry));
	const char name[] = "CaveWiiGameMode";
	char* node_name = malloc(sizeof(name));
	if(!node || !entry || !node_name) {
		free(node);
		free(entry);
		free(node_name);
		return false;
	}

	memcpy(node_name, name, sizeof(name));
	node->type = TAG_BYTE;
	node->name = node_name;
	node->payload.tag_byte = value;
	entry->data = node;
	list_add_tail(&entry->entry, &data->payload.tag_compound->entry);
	la->modified = true;
	return true;
}

bool level_archive_read_death_tx_counter(struct level_archive* la,
										 uint32_t* out) {
	assert(la && la->data && out);
	nbt_node* node
		= nbt_find_by_path(la->data, LEVEL_CAVEWII_DEATH_TX_COUNTER.name);
	if(!node || node->type != TAG_LONG)
		return false; /* absent (legacy) or malformed: caller seeds the floor */
	/* Range hardening (adversarial review): legitimately persisted values are
	 * always >= the legacy floor (the allocator persists floor+1 upward and
	 * recovery only bumps), and UINT32_MAX would wrap the allocator to 1 AND
	 * persist the regression. Anything outside that band is hand-edited or
	 * corrupt -- read as absent so the caller re-seeds the safe floor. */
	if(node->payload.tag_long < (int64_t)CAVEWII_DEATH_TX_LEGACY_FLOOR
	   || node->payload.tag_long >= (int64_t)UINT32_MAX)
		return false;
	*out = (uint32_t)node->payload.tag_long;
	return true;
}

bool level_archive_write_death_tx_counter(struct level_archive* la,
										  uint32_t value) {
	assert(la && la->data);

	nbt_node* node
		= nbt_find_by_path(la->data, LEVEL_CAVEWII_DEATH_TX_COUNTER.name);
	if(node) {
		/* CaveWii owns this tag and the reader already discards a malformed
		 * payload as absent -- coerce a wrong-type node instead of wedging
		 * every future write against it (adversarial review: a permanent
		 * write failure would floor-seed every session and deterministically
		 * reissue ids against on-disk tags). */
		node->type = TAG_LONG;
		node->payload.tag_long = (int64_t)value;
		la->modified = true;
		return true;
	}

	/* Legacy/imported worlds predate the counter. Allocate the entire list
	 * entry before linking (the FallDistance migration pattern below) so OOM
	 * leaves the live archive byte-for-byte unchanged. */
	nbt_node* data = nbt_find_by_path(la->data, ".Data");
	if(!data || data->type != TAG_COMPOUND || !data->payload.tag_compound)
		return false;

	node = calloc(1, sizeof(*node));
	struct nbt_list* entry = malloc(sizeof(*entry));
	const char name[] = "CaveWiiDeathTxCounter";
	char* node_name = malloc(sizeof(name));
	if(!node || !entry || !node_name) {
		free(node);
		free(entry);
		free(node_name);
		return false;
	}

	memcpy(node_name, name, sizeof(name));
	node->type = TAG_LONG;
	node->name = node_name;
	node->payload.tag_long = (int64_t)value;
	entry->data = node;
	list_add_tail(&entry->entry, &data->payload.tag_compound->entry);
	la->modified = true;
	return true;
}

bool level_archive_write_player_fall_distance(struct level_archive* la,
										 float fall_distance) {
	assert(la && la->data);

	if(!isfinite(fall_distance) || fall_distance < 0.0F)
		return false;

	nbt_node* node
		= nbt_find_by_path(la->data, LEVEL_PLAYER_FALL_DISTANCE.name);
	if(node) {
		if(node->type != TAG_FLOAT)
			return false;
		node->payload.tag_float = fall_distance;
		la->modified = true;
		return true;
	}

	/* Legacy CaveWii/Beta trees have Data.Player but predate FallDistance.
	 * Allocate the entire list entry before linking it so OOM leaves the live
	 * archive byte-for-byte unchanged. */
	nbt_node* player = nbt_find_by_path(la->data, ".Data.Player");
	if(!player || player->type != TAG_COMPOUND
	   || !player->payload.tag_compound)
		return false;

	node = calloc(1, sizeof(*node));
	struct nbt_list* entry = malloc(sizeof(*entry));
	const char name[] = "FallDistance";
	char* node_name = malloc(sizeof(name));
	if(!node || !entry || !node_name) {
		free(node);
		free(entry);
		free(node_name);
		return false;
	}

	memcpy(node_name, name, sizeof(name));
	node->type = TAG_FLOAT;
	node->name = node_name;
	node->payload.tag_float = fall_distance;
	entry->data = node;
	list_add_tail(&entry->entry, &player->payload.tag_compound->entry);
	la->modified = true;
	return true;
}

static int level_player_spawn_name_index(const char* name) {
	if(!name)
		return -1;
	if(strcmp(name, "SpawnX") == 0)
		return 0;
	if(strcmp(name, "SpawnY") == 0)
		return 1;
	if(strcmp(name, "SpawnZ") == 0)
		return 2;
	return -1;
}

static nbt_node* level_player_compound(struct level_archive* la) {
	nbt_node* player = nbt_find_by_path(la->data, ".Data.Player");
	return player && player->type == TAG_COMPOUND
		&& player->payload.tag_compound ? player : NULL;
}

static void level_player_spawn_nodes(nbt_node* player, nbt_node* nodes[3],
									 unsigned counts[3]) {
	memset(nodes, 0, sizeof(nbt_node*) * 3);
	memset(counts, 0, sizeof(unsigned) * 3);
	struct list_head* current;
	list_for_each(current, &player->payload.tag_compound->entry) {
		struct nbt_list* entry = list_entry(current, struct nbt_list, entry);
		int index = entry && entry->data
			? level_player_spawn_name_index(entry->data->name) : -1;
		if(index >= 0) {
			counts[index]++;
			if(counts[index] == 1)
				nodes[index] = entry->data;
		}
	}
}

static bool level_player_spawn_values_valid(int32_t x, int32_t y, int32_t z) {
	return x >= -(int32_t)LEVEL_PLAYER_XZ_LIMIT
		&& x <= (int32_t)LEVEL_PLAYER_XZ_LIMIT && z >= -(int32_t)LEVEL_PLAYER_XZ_LIMIT
		&& z <= (int32_t)LEVEL_PLAYER_XZ_LIMIT && y >= 0 && y < WORLD_HEIGHT;
}

bool level_archive_read_player_spawn(struct level_archive* la, bool* present,
									 int32_t* x, int32_t* y, int32_t* z) {
	assert(la && la->data && present && x && y && z);
	*present = false;
	nbt_node* player = level_player_compound(la);
	if(!player)
		return false;
	nbt_node* nodes[3];
	unsigned counts[3];
	level_player_spawn_nodes(player, nodes, counts);
	if(counts[0] == 0 && counts[1] == 0 && counts[2] == 0)
		return true;
	if(counts[0] != 1 || counts[1] != 1 || counts[2] != 1
	   || nodes[0]->type != TAG_INT || nodes[1]->type != TAG_INT
	   || nodes[2]->type != TAG_INT)
		return false;
	int32_t parsed_x = nodes[0]->payload.tag_int;
	int32_t parsed_y = nodes[1]->payload.tag_int;
	int32_t parsed_z = nodes[2]->payload.tag_int;
	if(!level_player_spawn_values_valid(parsed_x, parsed_y, parsed_z))
		return false;
	*x = parsed_x;
	*y = parsed_y;
	*z = parsed_z;
	*present = true;
	return true;
}

static void level_player_spawn_remove_all(nbt_node* player) {
	struct list_head *current, *next;
	list_for_each_safe(current, next, &player->payload.tag_compound->entry) {
		struct nbt_list* entry = list_entry(current, struct nbt_list, entry);
		if(entry && entry->data
		   && level_player_spawn_name_index(entry->data->name) >= 0) {
			list_del(current);
			nbt_free(entry->data);
			free(entry);
		}
	}
}

static bool level_player_spawn_alloc_entry(struct nbt_list** out,
										 const char* name, int32_t value) {
	struct nbt_list* entry = malloc(sizeof(*entry));
	nbt_node* node = calloc(1, sizeof(*node));
	char* node_name = malloc(strlen(name) + 1);
	if(!entry || !node || !node_name) {
		free(entry);
		free(node);
		free(node_name);
		return false;
	}
	memcpy(node_name, name, strlen(name) + 1);
	node->type = TAG_INT;
	node->name = node_name;
	node->payload.tag_int = value;
	entry->data = node;
	*out = entry;
	return true;
}

bool level_archive_write_player_spawn(struct level_archive* la, bool present,
									  int32_t x, int32_t y, int32_t z) {
	assert(la && la->data);
	if(present && !level_player_spawn_values_valid(x, y, z))
		return false;
	nbt_node* player = level_player_compound(la);
	if(!player)
		return false;
	nbt_node* nodes[3];
	unsigned counts[3];
	level_player_spawn_nodes(player, nodes, counts);
	bool any = counts[0] || counts[1] || counts[2];
	if(!present) {
		if(any) {
			level_player_spawn_remove_all(player);
			la->modified = true;
		}
		return true;
	}
	if(counts[0] == 1 && counts[1] == 1 && counts[2] == 1
	   && nodes[0]->type == TAG_INT && nodes[1]->type == TAG_INT
	   && nodes[2]->type == TAG_INT) {
		if(nodes[0]->payload.tag_int != x || nodes[1]->payload.tag_int != y
		   || nodes[2]->payload.tag_int != z) {
			nodes[0]->payload.tag_int = x;
			nodes[1]->payload.tag_int = y;
			nodes[2]->payload.tag_int = z;
			la->modified = true;
		}
		return true;
	}

	/* Build the full replacement before unlinking a malformed/absent old triple. */
	static const char* names[3] = {"SpawnX", "SpawnY", "SpawnZ"};
	int32_t values[3] = {x, y, z};
	struct nbt_list* replacements[3] = {NULL, NULL, NULL};
	for(size_t k = 0; k < 3; k++)
		if(!level_player_spawn_alloc_entry(
			   &replacements[k], names[k], values[k])) {
			for(size_t n = 0; n < 3; n++)
				if(replacements[n]) {
					nbt_free(replacements[n]->data);
					free(replacements[n]);
				}
			return false;
		}
	level_player_spawn_remove_all(player);
	for(size_t k = 0; k < 3; k++)
		list_add_tail(&replacements[k]->entry,
					  &player->payload.tag_compound->entry);
	la->modified = true;
	return true;
}

bool level_archive_read_inventory(struct level_archive* la,
								  struct inventory* inventory) {
	assert(la && inventory);
	assert(la->data);

	nbt_node* inv = nbt_find_by_path(la->data, LEVEL_PLAYER_INVENTORY.name);
	if(!inv)
		return false;

	/* CR-02, docs/audit_2026-08-20_code_review.md: TRANSACTIONAL parse. The old
	 * code called inventory_clear() on the LIVE inventory up front and then wrote
	 * each slot as the NBT list was walked, so a malformed or LATE-invalid entry
	 * (anywhere up to and including the last one) left the live inventory cleared
	 * or half-populated. The caller ignored the return value, so a later
	 * checkpoint (server_local_write_level_state -> level_archive_write_inventory)
	 * persisted that partial state, permanently erasing the player's items.
	 * Parse into a TEMPORARY buffer sized to this inventory's capacity, validate
	 * the ENTIRE list first, and only on full success replace the live inventory
	 * in one shot. On ANY failure the live inventory (items, picked item, hotbar
	 * selection) is left exactly as it was. */
	struct item_data* staged
		= calloc(inventory->capacity, sizeof(struct item_data));
	if(!staged)
		return false;

	if(!item_nbt_read_list(inv, staged, inventory->capacity, true)) {
		free(staged);
		return false;
	}

	// COMMIT (CR-02): the whole list validated -- replace the live inventory in
	// one shot. inventory_clear() reproduces the old function's success-path
	// semantics exactly (zeroes items/picked_item/hotbar_slot), then every slot
	// present in the archive is copied in from `staged`.
	inventory_clear(inventory);
	memcpy(inventory->items, staged,
		  inventory->capacity * sizeof(struct item_data));
	free(staged);

	return true;
}

bool level_archive_write_inventory(struct level_archive* la,
								   struct inventory* inventory) {
	assert(la && inventory);
	assert(la->data);

	la->inventory_write_refused = true;

	nbt_node* inv = nbt_find_by_path(la->data, LEVEL_PLAYER_INVENTORY.name);
	if(!inv)
		return false;

	/* Build off-tree. Pinned records are checked against the original list;
	 * any refusal leaves both the NBT tree and the previous file intact. */
	const nbt_node* basis=inv;
	/* Preserve the existing CaveWii writer's runtime-slot ordering, but only
	 * after proving that the old list was exactly its canonical encoding. */
	if(inventory->capacity==INVENTORY_SIZE && level_archive_was_created_by_cavewii(la)) {
		struct item_data before[INVENTORY_SIZE]={0};
		bool canonical=item_nbt_read_list(inv,before,INVENTORY_SIZE,true);
		for(size_t k=0;k<INVENTORY_SIZE;k++)
			if(before[k].nbt_record || inventory->items[k].nbt_record) canonical=false;
		if(canonical) {
			nbt_node* baseline=item_nbt_write_list(NULL,before,INVENTORY_SIZE,true);
			if(baseline) {
				baseline->name=inv->name;
				if(item_nbt_equal(inv,baseline)) basis=NULL;
				baseline->name=NULL; nbt_free(baseline);
			}
		}
	}
	nbt_node* replacement = item_nbt_write_list(basis, inventory->items,
		inventory->capacity, true);
	if(!replacement) return false;
	nbt_node* original_bytes = item_nbt_clone_lossless(inv);
	struct buffer old_bytes = nbt_dump_binary(original_bytes);
	nbt_free(original_bytes);
	replacement->name = inv->name;
	struct buffer new_bytes = nbt_dump_binary(replacement);
	replacement->name = NULL;
	bool same = old_bytes.data && new_bytes.data && old_bytes.len == new_bytes.len
		&& !memcmp(old_bytes.data,new_bytes.data,old_bytes.len);
	bool ok = old_bytes.data && new_bytes.data;
	buffer_free(&old_bytes); buffer_free(&new_bytes);
	if(!ok) { nbt_free(replacement); return false; }
	nbt_node* previous=malloc(sizeof(*previous));
	if(!previous) { nbt_free(replacement); return false; }
	*previous=*inv; previous->name=NULL;
	char* retained_name=inv->name;
	*inv=*replacement; inv->name=retained_name;
	free(replacement); nbt_free(previous);
	if(!same) la->modified = true;
	la->inventory_write_refused = false;
	return true;
}

// could use float* instead of vec3, but not sure of possible alignment for vec3
// (right now this means we can read three values at most)
static bool read_vector(nbt_node* node, nbt_type type, vec3 result,
						size_t amount) {
	if(!node || node->type != TAG_LIST || !node->payload.tag_list || !result
	   || amount > 3)
		return false;

	size_t k = 0;
	vec3 parsed = {0.0f, 0.0f, 0.0f};
	struct list_head* current;
	list_for_each(current, &node->payload.tag_list->entry) {
		nbt_node* obj = list_entry(current, struct nbt_list, entry)->data;
		if(obj && obj->type == type && k < amount) {
			double value;
			switch(type) {
				case TAG_DOUBLE: value = obj->payload.tag_double; break;
				case TAG_FLOAT: value = obj->payload.tag_float; break;
				default: return false;
			}
			parsed[k] = (float)value;
			if(!isfinite(value) || !isfinite(parsed[k]))
				return false;
			k++;
		} else {
			return false;
		}
	}

	if(k != amount)
		return false;
	memcpy(result, parsed, amount * sizeof(float));
	return true;
}

static bool vector_shape_valid(const nbt_node* node, nbt_type type,
							   size_t amount) {
	if(!node || node->type != TAG_LIST || !node->payload.tag_list || amount > 3)
		return false;
	size_t count = 0;
	struct list_head* current;
	list_for_each(current, &node->payload.tag_list->entry) {
		const nbt_node* obj = list_entry(current, struct nbt_list, entry)->data;
		if(!obj || obj->type != type || count >= amount)
			return false;
		count++;
	}
	return count == amount;
}

static bool write_vector(nbt_node* node, nbt_type type, vec3 data,
						 size_t amount) {
	if(!data || !vector_shape_valid(node, type, amount))
		return false;
	for(size_t k = 0; k < amount; k++)
		if(!isfinite(data[k]))
			return false;

	size_t k = 0;
	struct list_head* current;
	list_for_each(current, &node->payload.tag_list->entry) {
		nbt_node* obj = list_entry(current, struct nbt_list, entry)->data;
		if(obj && obj->type == type && k < amount) {
			switch(type) {
				case TAG_DOUBLE: obj->payload.tag_double = data[k++]; break;
				case TAG_FLOAT: obj->payload.tag_float = data[k++]; break;
				default: return false;
			}
		} else {
			return false;
		}
	}

	return k == amount;
}

bool level_player_position_values_valid(double x, double y, double z) {
	return isfinite(x) && isfinite(y) && isfinite(z)
		&& fabs(x) <= LEVEL_PLAYER_XZ_LIMIT && fabs(y) <= LEVEL_PLAYER_Y_LIMIT
		&& fabs(z) <= LEVEL_PLAYER_XZ_LIMIT;
}

static bool player_position_valid(const vec3 position) {
	return position && level_player_position_values_valid(
		position[0], position[1], position[2]);
}

bool level_player_velocity_values_valid(float x, float y, float z) {
	return isfinite(x) && isfinite(y) && isfinite(z)
		&& fabsf(x) <= LEVEL_PLAYER_VELOCITY_LIMIT
		&& fabsf(y) <= LEVEL_PLAYER_VELOCITY_LIMIT
		&& fabsf(z) <= LEVEL_PLAYER_VELOCITY_LIMIT;
}

static bool player_velocity_valid(const vec3 velocity) {
	return velocity && level_player_velocity_values_valid(
		velocity[0], velocity[1], velocity[2]);
}

bool level_player_rotation_values_valid(float x, float y) {
	return isfinite(x) && isfinite(y);
}

static bool player_rotation_valid(const vec2 rotation) {
	return rotation
		&& level_player_rotation_values_valid(rotation[0], rotation[1]);
}

/* IMP-SAVE-NO-PLAYER: an imported Beta level.dat may carry no Data.Player
   (adventure maps). Beta's WorldInfo.updateTagCompound always sets a full
   "Player" compound on save, so graft the template's Player under .Data when
   it is absent; the normal writers then fill in the live values. Everything
   is allocated before linking, so failure leaves the tree unchanged. */
static bool ensure_player_compound(struct level_archive* la) {
	nbt_node* data = nbt_find_by_path(la->data, ".Data");
	if(!data || data->type != TAG_COMPOUND || !data->payload.tag_compound)
		return false;
	if(nbt_find_by_path(la->data, ".Data.Player"))
		return true;
	nbt_node* tmpl = nbt_parse_compressed(LEVEL_DAT_TEMPLATE, sizeof(LEVEL_DAT_TEMPLATE));
	if(!tmpl)
		return false;
	nbt_node* src = nbt_find_by_path(tmpl, ".Data.Player");
	nbt_node* copy = src ? nbt_clone(src) : NULL;
	nbt_free(tmpl);
	if(!copy)
		return false;
	struct nbt_list* entry = calloc(1, sizeof(*entry));
	if(!entry) {
		nbt_free(copy);
		return false;
	}
	entry->data = copy;
	list_add_tail(&entry->entry, &data->payload.tag_compound->entry);
	return true;
}

bool level_archive_write_player(struct level_archive* la, vec3 position,
								vec2 rotation, vec3 velocity,
								enum world_dim dimension) {
	assert(la && la->data);
	if((position && !player_position_valid(position))
	   || (rotation && !player_rotation_valid(rotation))
	   || (velocity && !player_velocity_valid(velocity)))
		return false;

	if(!ensure_player_compound(la))
		return false;

	nbt_node* pos;
	if(!level_archive_read(la, LEVEL_PLAYER_POSITION, &pos, 0))
		return false;

	nbt_node* vel;
	if(!level_archive_read(la, LEVEL_PLAYER_VELOCITY, &vel, 0))
		return false;

	nbt_node* rot;
	if(!level_archive_read(la, LEVEL_PLAYER_ROTATION, &rot, 0))
		return false;

	nbt_node* dim_node = nbt_find_by_path(la->data, LEVEL_PLAYER_DIMENSION.name);
	if(!dim_node || dim_node->type != TAG_INT)
		return false;

	// Defense-in-depth (cf. amahpour/CavEX #51): refuse to PERSIST a corrupted at-origin/void
	// player position over a valid saved spawn. An in-memory position snapped to ~(0,0,0) with
	// y<2 (from any future origin-snap-class bug) would otherwise overwrite level.dat's real
	// spawn and stick across every later load. We skip ONLY the position write -- keeping the
	// last valid spawn -- and still persist rotation/velocity/dimension. This is a SAFETY NET,
	// NOT a root fix: if origin-snap ever manifests here, its cause must still be found.
	bool persist_pos = position
		&& !(position[0] > -1.5F && position[0] < 1.5F && position[2] > -1.5F
			 && position[2] < 1.5F && position[1] < 2.0F);
	if((persist_pos && !vector_shape_valid(pos, TAG_DOUBLE, 3))
	   || (velocity && !vector_shape_valid(vel, TAG_DOUBLE, 3))
	   || (rotation && !vector_shape_valid(rot, TAG_FLOAT, 2)))
		return false;
	if(persist_pos && !write_vector(pos, TAG_DOUBLE, position, 3))
		return false;

	if(velocity && !write_vector(vel, TAG_DOUBLE, velocity, 3))
		return false;

	if(rotation
	   && !write_vector(rot, TAG_FLOAT, (vec3) {rotation[0], rotation[1], 0.0F},
						2))
		return false;

	/* Commit the scalar last, after every vector preflight/write has succeeded. */
	dim_node->payload.tag_int = dimension;

	la->modified = true;

	return true;
}

/* M1 (Fable A2T-SERVER-REVIEW): the raw Player pose/dimension payloads, copied
 * verbatim (doubles stay doubles -- no float round trip), so a caller that
 * writes a record and then fails to publish it can put the tree back exactly. */
static bool raw_vector(nbt_node* node, nbt_type type, void* out, size_t amount,
					   bool store) {
	if(!vector_shape_valid(node, type, amount))
		return false;
	size_t k = 0;
	struct list_head* current;
	list_for_each(current, &node->payload.tag_list->entry) {
		nbt_node* obj = list_entry(current, struct nbt_list, entry)->data;
		if(type == TAG_DOUBLE) {
			double* d = out;
			if(store)
				obj->payload.tag_double = d[k];
			else
				d[k] = obj->payload.tag_double;
		} else {
			float* f = out;
			if(store)
				obj->payload.tag_float = f[k];
			else
				f[k] = obj->payload.tag_float;
		}
		k++;
	}
	return k == amount;
}

bool level_archive_player_record_snapshot(
	struct level_archive* la, struct level_archive_player_record* rec) {
	assert(la && la->data && rec);
	memset(rec, 0, sizeof(*rec));
	nbt_node *pos, *rot;
	nbt_node* dim_node = nbt_find_by_path(la->data, LEVEL_PLAYER_DIMENSION.name);
	if(!level_archive_read(la, LEVEL_PLAYER_POSITION, &pos, 0)
	   || !level_archive_read(la, LEVEL_PLAYER_ROTATION, &rot, 0) || !dim_node
	   || dim_node->type != TAG_INT || !raw_vector(pos, TAG_DOUBLE, rec->pos, 3, false)
	   || !raw_vector(rot, TAG_FLOAT, rec->rot, 2, false))
		return false;
	rec->dimension = dim_node->payload.tag_int;
	rec->modified = la->modified;
	rec->valid = true;
	return true;
}

bool level_archive_player_record_restore(
	struct level_archive* la, const struct level_archive_player_record* rec) {
	assert(la && la->data && rec);
	if(!rec->valid)
		return false;
	nbt_node *pos, *rot;
	nbt_node* dim_node = nbt_find_by_path(la->data, LEVEL_PLAYER_DIMENSION.name);
	if(!level_archive_read(la, LEVEL_PLAYER_POSITION, &pos, 0)
	   || !level_archive_read(la, LEVEL_PLAYER_ROTATION, &rot, 0) || !dim_node
	   || dim_node->type != TAG_INT || !vector_shape_valid(pos, TAG_DOUBLE, 3)
	   || !vector_shape_valid(rot, TAG_FLOAT, 2))
		return false;
	double p[3];
	float r[2];
	memcpy(p, rec->pos, sizeof(p));
	memcpy(r, rec->rot, sizeof(r));
	(void)raw_vector(pos, TAG_DOUBLE, p, 3, true);
	(void)raw_vector(rot, TAG_FLOAT, r, 2, true);
	dim_node->payload.tag_int = rec->dimension;
	la->modified = rec->modified;
	return true;
}

/* PLAYER-HEALTH-NBT (2026-10-25). Beta stores the player's health as
 * EntityLiving.health, an int in HALF-HEARTS (EntityPlayer ctor
 * health 20, EntityPlayer.java:47), written as
 * the short tag "Health" (EntityLiving.java:558) and
 * read back from it (EntityLiving.java:565).
 * CaveWii's internal scale is HEALTH_PER_HEART (16) per heart, i.e. 8 units
 * per Beta half-heart (MAX_PLAYER_HEALTH 160 == Beta 20). Before this fix the
 * internal value was written and read verbatim, so a real b1.7.3 client
 * loaded a full CaveWii player at 160 half-hearts (beta-trial-1025 observed
 * 120 = 6x), and an imported Beta world's 20 loaded at 1/8 health.
 *
 * Health is now ALWAYS Beta half-hearts on disk. The exact internal value
 * rides alongside in the CaveWii-only Data.Player.CaveWiiHealth (TAG_SHORT;
 * Beta's WorldInfo/EntityPlayer writers rebuild the compound, so a Beta
 * client drops it). It is honoured only while it still converts to the
 * stored Health, so any external edit of Health wins. */
#define LEVEL_PLAYER_CAVEWII_HEALTH_NAME ".Data.Player.CaveWiiHealth"

int16_t level_archive_health_to_beta(int16_t units) {
	/* Beta's health is an integer half-heart count and every Beta damage /
	 * heal step is a whole half-heart, so a faithful CaveWii value is a
	 * multiple of 8 and this is exact. A non-multiple (a port-side
	 * fractional state) rounds AWAY from the death threshold for a living
	 * player: ceil keeps any units > 0 at >= 1 half-heart (alive in Beta,
	 * whose death gate is health <= 0), and units <= 0 truncate toward zero
	 * so a dead player stays <= 0. CaveWiiHealth keeps the exact value for
	 * CaveWii's own round trip. */
	const int32_t per_half = HEALTH_PER_HEART / 2;
	int32_t u = units;
	int32_t half = u > 0 ? (u + per_half - 1) / per_half : u / per_half;
	return (int16_t)half;
}

int16_t level_archive_health_from_beta(int16_t half) {
	/* int32 math: a hostile/edited 32767 overflows int16 at x8. Clamp to
	 * MAX_PLAYER_HEALTH like health_wire_to_internal; keep the sign for <= 0
	 * so a Beta quit-while-dead save (Health 0 or negative) still loads dead
	 * and the guest reader can still reject a negative value. */
	int32_t u = (int32_t)half * (HEALTH_PER_HEART / 2);
	if(u > MAX_PLAYER_HEALTH)
		u = MAX_PLAYER_HEALTH;
	if(u < INT16_MIN)
		u = INT16_MIN;
	return (int16_t)u;
}

/* Allocate a named TAG_SHORT node (not yet linked). */
static nbt_node* level_new_short(const char* name, int16_t value) {
	nbt_node* node = calloc(1, sizeof(*node));
	size_t n = strlen(name) + 1;
	char* node_name = malloc(n);
	if(!node || !node_name) {
		free(node);
		free(node_name);
		return NULL;
	}
	memcpy(node_name, name, n);
	node->type = TAG_SHORT;
	node->name = node_name;
	node->payload.tag_short = value;
	return node;
}

/* Set Health (half-hearts) and CaveWiiHealth (exact units) together under
 * Data.Player, creating either if absent. Every node and list entry is
 * allocated before anything is linked or overwritten, so failure leaves the
 * tree byte-for-byte unchanged. A present wrong-type tag is corruption. */
static bool level_set_player_health_pair(struct level_archive* la,
										 int16_t beta_half, int16_t units) {
	nbt_node* player = level_player_compound(la);
	if(!player)
		return false;
	nbt_node* health = nbt_find_by_path(la->data, LEVEL_PLAYER_HEALTH.name);
	nbt_node* exact
		= nbt_find_by_path(la->data, LEVEL_PLAYER_CAVEWII_HEALTH_NAME);
	if((health && health->type != TAG_SHORT)
	   || (exact && exact->type != TAG_SHORT))
		return false;
	nbt_node* new_health = health ? NULL : level_new_short("Health", 0);
	nbt_node* new_exact = exact ? NULL : level_new_short("CaveWiiHealth", 0);
	struct nbt_list* he = health ? NULL : malloc(sizeof(*he));
	struct nbt_list* ee = exact ? NULL : malloc(sizeof(*ee));
	if((!health && (!new_health || !he)) || (!exact && (!new_exact || !ee))) {
		if(new_health)
			nbt_free(new_health);
		if(new_exact)
			nbt_free(new_exact);
		free(he);
		free(ee);
		return false;
	}
	if(!health) {
		he->data = health = new_health;
		list_add_tail(&he->entry, &player->payload.tag_compound->entry);
	}
	if(!exact) {
		ee->data = exact = new_exact;
		list_add_tail(&ee->entry, &player->payload.tag_compound->entry);
	}
	health->payload.tag_short = beta_half;
	exact->payload.tag_short = units;
	la->modified = true;
	return true;
}

/* Unlink and free every Data.Player child named `name`. */
static void level_player_remove_child(nbt_node* player, const char* name) {
	struct list_head* cur;
	struct list_head* tmp;
	list_for_each_safe(cur, tmp, &player->payload.tag_compound->entry) {
		struct nbt_list* e = list_entry(cur, struct nbt_list, entry);
		if(e->data && e->data->name && !strcmp(e->data->name, name)) {
			list_del(cur);
			nbt_free(e->data);
			free(e);
		}
	}
}

bool level_archive_write_player_health(struct level_archive* la,
									   int16_t units) {
	assert(la && la->data);
	return level_set_player_health_pair(
		la, level_archive_health_to_beta(units), units);
}

/* Units of a MARKER-LESS (pre-fix) Player.Health. Provenance is read from
 * the Player compound itself, because CaveWii edits the parsed tree in place
 * (unknown tags survive) while Beta rebuilds it from scratch:
 *   - Beta 1.7.3 ALWAYS writes Sleeping + SleepTimer (EntityPlayer.java:330-
 *     331, unconditional) and Score (EntityPlayerSP.java:100); pre-1.3 Beta
 *     and Alpha still write Score. No CaveWii level template (every revision
 *     since world creation landed in a4b192b5c, 2026-05-31) and no CaveWii
 *     writer ever emits any of the three. So "has a Beta-client tag" = the
 *     compound was born in Beta; "has none" = born in a CaveWii template.
 *   - CaveWii* Data tags are NOT used: a pre-fix import is stamped with
 *     CaveWiiSaveVersion at load whether or not a checkpoint followed, and
 *     pre-June-2026 CaveWii worlds carry no tag at all.
 * Table (h = stored Health, no consistent CaveWiiHealth marker):
 *   Beta-born, h > 20  -> CaveWii units. Beta cannot hold > 20: heal() caps
 *       at 20 (EntityLiving.java:299-300), spawn/respawn set 20
 *       (EntityPlayer.java:47/154); only a CaveWii checkpoint writes more.
 *   Beta-born, h <= 20 -> Beta half-hearts. Exact for a real Beta save and
 *       for an import a pre-fix build stamped but never checkpointed.
 *       AMBIGUOUS with an import a pre-fix build DID checkpoint at <= 20
 *       units; those builds read the Beta value verbatim (the 1/8 bug), so
 *       x8 restores it, at worst over-healing up to full (never above).
 *   Template-born, h == 20 -> Beta half-hearts (full). The 2026-05-31..07-27
 *       templates stored a raw 20 (34e508998 and earlier); a world created
 *       then and never checkpointed holds it. AMBIGUOUS with a checkpointed
 *       20 units (1.25 hearts); resolved to full health.
 *   Template-born, other h -> CaveWii units (every CaveWii checkpoint since
 *       1af2471ca/adbd63543, 2024, wrote internal units; templates since
 *       f74e4d9d1 hold 160). */
static int16_t level_player_legacy_health_units(nbt_node* player,
												int16_t stored) {
	bool beta_born = false;
	struct list_head* cur;
	list_for_each(cur, &player->payload.tag_compound->entry) {
		nbt_node* n = list_entry(cur, struct nbt_list, entry)->data;
		if(n && n->name
		   && (!strcmp(n->name, "Score") || !strcmp(n->name, "Sleeping")
			   || !strcmp(n->name, "SleepTimer")))
			beta_born = true;
	}
	const int16_t beta_max = MAX_PLAYER_HEALTH / (HEALTH_PER_HEART / 2);
	if(beta_born)
		return stored > beta_max ? stored
								 : level_archive_health_from_beta(stored);
	return stored == beta_max ? MAX_PLAYER_HEALTH : stored;
}

bool level_archive_read_player_health_strict(struct level_archive* la,
											 bool legacy_units_without_marker,
											 int16_t* out) {
	assert(la && la->data && out);
	nbt_node* player = level_player_compound(la);
	nbt_node* health = nbt_find_by_path(la->data, LEVEL_PLAYER_HEALTH.name);
	if(!player || !health || health->type != TAG_SHORT)
		return false;
	int16_t stored = health->payload.tag_short;
	nbt_node* exact
		= nbt_find_by_path(la->data, LEVEL_PLAYER_CAVEWII_HEALTH_NAME);
	if(exact && exact->type == TAG_SHORT
	   && level_archive_health_to_beta(exact->payload.tag_short) == stored) {
		*out = exact->payload.tag_short;
		return true;
	}
	if(exact) {
		/* Stale/wrong-type marker: Health was edited after CaveWii wrote it
		 * (CaveWii writes both together), so Health is half-hearts. */
		*out = level_archive_health_from_beta(stored);
		return true;
	}
	/* No marker: a pre-fix record. Guest records are CaveWii-only. */
	*out = legacy_units_without_marker
		? stored
		: level_player_legacy_health_units(player, stored);
	return true;
}

bool level_archive_normalize_player_health(struct level_archive* la,
										   bool* converted) {
	assert(la && la->data);
	if(converted)
		*converted = false;
	nbt_node* player = level_player_compound(la);
	nbt_node* health = nbt_find_by_path(la->data, LEVEL_PLAYER_HEALTH.name);
	/* No Player / no (or wrong-type) Health: the reader's Beta defaults
	 * apply and the next checkpoint writes a correct pair. */
	if(!player || !health || health->type != TAG_SHORT)
		return true;
	nbt_node* exact
		= nbt_find_by_path(la->data, LEVEL_PLAYER_CAVEWII_HEALTH_NAME);
	int16_t stored = health->payload.tag_short;
	int16_t units;
	(void)level_archive_read_player_health_strict(la, false, &units);
	if(exact && exact->type == TAG_SHORT && exact->payload.tag_short == units
	   && level_archive_health_to_beta(units) == stored)
		return true; /* already a consistent post-fix pair */
	if(exact) {
		level_player_remove_child(player, "CaveWiiHealth");
		la->modified = true;
	}
	/* Persist the decision once (marker + Beta Health): every later load
	 * reads the exact pair and never re-classifies. */
	int16_t beta = level_archive_health_to_beta(units);
	if(!level_set_player_health_pair(la, beta, units))
		return false;
	if(converted)
		*converted = beta != stored;
	return true;
}

bool level_archive_read_player_health(struct level_archive* la, int16_t* out) {
	assert(la && la->data && out);
	nbt_node* player = nbt_find_by_path(la->data, ".Data.Player");
	/* World.spawnPlayerWithLoadedChunks: readFromNBT only when the tag exists;
	 * otherwise the new EntityPlayer keeps health 20. */
	if(!player || player->type != TAG_COMPOUND) {
		*out = MAX_PLAYER_HEALTH;
		return true;
	}
	/* EntityLiving.readEntityFromNBT: !hasKey("Health") -> health = 10. */
	if(!level_archive_read_player_health_strict(la, false, out))
		*out = MAX_PLAYER_HEALTH / 2;
	return true;
}

bool level_archive_read_player(struct level_archive* la, vec3 position,
							   vec2 rotation, vec3 velocity,
							   enum world_dim* dimension) {
	assert(la && la->data);

	nbt_node* pos;
	if(!level_archive_read(la, LEVEL_PLAYER_POSITION, &pos, 0))
		return false;

	nbt_node* vel;
	if(!level_archive_read(la, LEVEL_PLAYER_VELOCITY, &vel, 0))
		return false;

	nbt_node* rot;
	if(!level_archive_read(la, LEVEL_PLAYER_ROTATION, &rot, 0))
		return false;

	int32_t dim = 0; // = overworld; only read when `dimension` is requested
	if(dimension && !level_archive_read(la, LEVEL_PLAYER_DIMENSION, &dim, 0))
		return false;

	vec3 parsed_position, parsed_velocity, parsed_rotation;
	if(position
	   && (!read_vector(pos, TAG_DOUBLE, parsed_position, 3)
		   || !player_position_valid(parsed_position)))
		return false;
	if(velocity
	   && (!read_vector(vel, TAG_DOUBLE, parsed_velocity, 3)
		   || !player_velocity_valid(parsed_velocity)))
		return false;
	if(rotation) {
		if(!read_vector(rot, TAG_FLOAT, parsed_rotation, 2)
		   || !player_rotation_valid(
			   (vec2) {parsed_rotation[0], parsed_rotation[1]}))
			return false;
	}

	/* Commit caller outputs only after every requested vector validated. */
	if(position)
		memcpy(position, parsed_position, sizeof(parsed_position));
	if(velocity)
		memcpy(velocity, parsed_velocity, sizeof(parsed_velocity));
	if(rotation) {
		rotation[0] = parsed_rotation[0];
		rotation[1] = parsed_rotation[1];
	}

	// ensures output is valid dimension
	if(dimension)
		*dimension = (dim == 0) ? WORLD_DIM_OVERWORLD : WORLD_DIM_NETHER;

	return true;
}

// Write the tree to disk via Beta's safe rotation (SaveHandler does the same
// dance): dump to level.dat_new, rotate the current file to level.dat_old,
// rename _new into place; load validates primary, _new, then _old. This is
// recovery across ordered file operations, not an atomic FAT directory-sector
// transaction. See the slice-2 document for the storage model and limitations.
/* G9 R19: level_archive_flush, split at its two thread boundaries so the
 * deferred IO worker can run the middle -- byte for byte the same code.
 *   prepare (server):  the lossless clone of the live tree and the
 *                      unchanged-image compare; the clone is IMMUTABLE from
 *                      here on and owned by whoever runs the write;
 *   write (any):       dump + fflush + fsync + fclose of level.dat_new, the
 *                      parse-back verify, the _old/_new rotation -- touches
 *                      only the clone and the path;
 *   install (server):  the verified image becomes the next save's compare
 *                      baseline. */
enum level_flush_prep { LEVEL_FLUSH_FAIL, LEVEL_FLUSH_UNCHANGED, LEVEL_FLUSH_WRITE };
static enum level_flush_prep level_archive_flush_prepare(struct level_archive* la,
													 nbt_node** out_exact) {
	const bool diag = true;
	(void)diag;
	*out_exact = NULL;
	if(la->lossless_blocked || la->inventory_write_refused) return LEVEL_FLUSH_FAIL;
	/* A checkpoint may update player/world fields without rewriting Inventory. */
	SAVE_DIAG(SERVER_DIAG_FLUSH_CLONE);
	nbt_node* exact = item_nbt_clone_lossless(la->data);
	if(!exact) return LEVEL_FLUSH_FAIL;
	nbt_node* retained = la->data->type == TAG_COMPOUND
		? la->data->payload.tag_compound->data : NULL;
	if(retained && retained->type == TAG_RAW_COMPOUND) {
		SAVE_DIAG(SERVER_DIAG_FLUSH_COMPARE);
		struct buffer bytes = nbt_dump_binary(exact);
		bool unchanged = bytes.data && retained->payload.tag_byte_array.length >= 0
			&& bytes.len == (size_t)retained->payload.tag_byte_array.length
			&& !memcmp(bytes.data, retained->payload.tag_byte_array.data, bytes.len);
		buffer_free(&bytes);
		if(unchanged) { nbt_free(exact); return LEVEL_FLUSH_UNCHANGED; }
	}
	*out_exact = exact;
	return LEVEL_FLUSH_WRITE;
}

static bool level_archive_flush_write(const nbt_node* exact_c,
									  const char* file_name,
									  nbt_node** out_snapshot, bool diag) {
	(void)diag;
	nbt_node* exact = (nbt_node*)exact_c; /* read only below */
	*out_snapshot = NULL;
#ifdef PLATFORM_WII
	save_path_set_parent(file_name);
#endif
	string_t tmp_name, old_name;
	string_init_printf(tmp_name, "%s_new", file_name);
	string_init_printf(old_name, "%s_old", file_name);

	bool ok = false;
	SAVE_DIAG(SERVER_DIAG_FLUSH_OPEN);
	FILE* f = fopen(string_get_cstr(tmp_name), "wb");
	if(f) {
		SAVE_DIAG(SERVER_DIAG_FLUSH_DUMP);
		ok = nbt_dump_file(exact, f, STRAT_GZIP) == NBT_OK;
		/* Durability barrier BEFORE the renames (the doctrine already applied
		 * to the journal and the region archives): fflush reaches only
		 * libfat's cache on the Wii; fsync (libfat f_sync) forces the bytes
		 * AND the FAT metadata to the card, so the rotation below never
		 * promotes a torn _new. */
		if(ok) {
			SAVE_DIAG(SERVER_DIAG_FLUSH_FFLUSH);
			if(fflush(f) != 0) ok = false;
		}
		if(ok) {
			SAVE_DIAG(SERVER_DIAG_FLUSH_FSYNC);
#ifdef _WIN32
			ok = _commit(_fileno(f)) == 0;
#else
			ok = fsync(fileno(f)) == 0;
#endif
		}
		SAVE_DIAG(SERVER_DIAG_FLUSH_CLOSE);
		if(fclose(f) != 0) // flush failure (disk full) surfaces here
			ok = false;
		if(ok)
			LEVEL_TRACE(RT_LEVEL_SYNC, file_name);
	}

	nbt_node* new_snapshot = NULL;
	if(ok) {
		SAVE_DIAG(SERVER_DIAG_FLUSH_PARSE);
		nbt_node* check = nbt_parse_path(string_get_cstr(tmp_name));
		SAVE_DIAG(SERVER_DIAG_FLUSH_CHECK_CLONE);
		nbt_node* check_exact = check ? item_nbt_clone_lossless(check) : NULL;
		SAVE_DIAG(SERVER_DIAG_FLUSH_EQUAL);
		ok = check_exact && level_archive_version_supported(check)
			&& item_nbt_equal(check_exact, exact);
		if(ok && check->type == TAG_COMPOUND) {
			new_snapshot = check->payload.tag_compound->data;
			check->payload.tag_compound->data = NULL;
		}
		nbt_free(check_exact); nbt_free(check);
	}
	if(ok) {
		// Rotation: _old may not exist (first save) and level.dat may not
		// exist (fresh world). A failed primary rotation gates publication.
		SAVE_DIAG(SERVER_DIAG_FLUSH_REMOVE_OLD);
		remove(string_get_cstr(old_name));
		SAVE_DIAG(SERVER_DIAG_FLUSH_RENAME_OLD);
		bool rotated = rename(file_name, string_get_cstr(old_name)) == 0;
		SAVE_DIAG(SERVER_DIAG_FLUSH_RENAME_NEW);
		ok = (rotated || errno == ENOENT) && rename(string_get_cstr(tmp_name), file_name)
			== 0;
		if(!ok) // restore the original so the world stays openable
			rename(string_get_cstr(old_name), file_name);
		if(ok)
			LEVEL_TRACE(RT_LEVEL_ROTATE, file_name);
	}

	SAVE_DIAG(SERVER_DIAG_FLUSH_CLEANUP);
	if(!ok)
		remove(string_get_cstr(tmp_name));

	if(ok)
		*out_snapshot = new_snapshot;
	else
		free(new_snapshot);
	string_clear(tmp_name);
	string_clear(old_name);
	return ok;
}

static void level_archive_flush_install(struct level_archive* la,
										nbt_node* new_snapshot) {
	if(new_snapshot) {
		/* Compare the NEXT save with the last committed image, not the image
		 * from initial load (A -> B -> A must really write A). */
		free(la->data->payload.tag_compound->data);
		la->data->payload.tag_compound->data = new_snapshot;
	}
}

static bool level_archive_flush(struct level_archive* la) {
	nbt_node* exact = NULL;
	enum level_flush_prep prep = level_archive_flush_prepare(la, &exact);
	if(prep != LEVEL_FLUSH_WRITE)
		return prep == LEVEL_FLUSH_UNCHANGED;
	nbt_node* new_snapshot = NULL;
	bool ok = level_archive_flush_write(exact, string_get_cstr(la->file_name),
										&new_snapshot, true);
	if(ok)
		level_archive_flush_install(la, new_snapshot);
	nbt_free(exact);
	return ok;
}

/* ---- G9 R19: the background (autosave) level.dat save ------------------- */
struct level_flush_job {
	nbt_node* exact;
	string_t file_name;
	nbt_node* new_snapshot;
	struct region_io_call* call;
};
static bool level_flush_job_run(void* ctx) {
	struct level_flush_job* job = ctx;
	return level_archive_flush_write(job->exact,
									 string_get_cstr(job->file_name),
									 &job->new_snapshot, false);
}
/* Take a queued save's outcome (server thread). `wait` makes it a barrier. */
static void level_archive_settle(struct level_archive* la, bool wait) {
	struct level_flush_job* job = la->pending_job;
	if(!job)
		return;
	bool ok = false;
	if(!wait && !region_archive_io_call_poll(job->call, &ok))
		return;
	ok = region_archive_io_call_wait(job->call);
	if(ok)
		level_archive_flush_install(la, job->new_snapshot);
	else {
		/* Re-dirty, never clear: a newer modification may already have set
		 * it, and the next save rewrites the CURRENT tree. */
		la->modified = true;
		nbt_free(job->new_snapshot);
	}
	nbt_free(job->exact);
	string_clear(job->file_name);
	free(job);
	la->pending_job = NULL;
}

void level_archive_settle_pending(struct level_archive* la) {
	if(la)
		level_archive_settle(la, true);
}

bool level_archive_save_background(struct level_archive* la) {
	assert(la && la->data);
	/* The prior save must have landed before this one compares against its
	 * image (A -> B -> A) or queues behind it. */
	level_archive_settle(la, true);
	if(!region_archive_deferred_io_enabled())
		return level_archive_save(la);
	if(la->lossless_blocked && la->modified)
		return false;
	if(!la->modified)
		return true;
	nbt_node* exact = NULL;
	enum level_flush_prep prep = level_archive_flush_prepare(la, &exact);
	if(prep == LEVEL_FLUSH_FAIL)
		return false;
	if(prep == LEVEL_FLUSH_UNCHANGED) {
		la->modified = false;
		la->lossless_blocked = false;
		la->inventory_write_refused = false;
		return true;
	}
	struct level_flush_job* job = calloc(1, sizeof(*job));
	if(job) {
		job->exact = exact;
		string_init_set(job->file_name, la->file_name);
		job->call = region_archive_io_call_submit(level_flush_job_run, job);
		if(!job->call) {
			string_clear(job->file_name);
			free(job);
			job = NULL;
		}
	}
	if(!job) {
		/* Cannot queue: the same save, synchronously. */
		nbt_node* new_snapshot = NULL;
		bool ok = level_archive_flush_write(exact, string_get_cstr(la->file_name),
											&new_snapshot, true);
		if(ok)
			level_archive_flush_install(la, new_snapshot);
		nbt_free(exact);
		if(!ok)
			return false;
		la->modified = false;
		la->lossless_blocked = false;
		la->inventory_write_refused = false;
		return true;
	}
	/* Queued (pending, NOT durable): clear optimistically, re-dirty on
	 * failure at the settle. Both refusal latches were already false (prepare
	 * refuses otherwise). */
	la->pending_job = job;
	la->modified = false;
	return true;
}

// Sum st_size for regular .mcr files directly inside `dir` (non-recursive --
// callers walk each subdirectory they care about themselves). Best-effort: a
// directory that doesn't exist (e.g. no Nether visited yet) contributes 0,
// never a failure. Portable stat()-based test, matching
// screen_select_world.c's world_dir_is_directory (dirent.d_type is not
// populated by every libc, notably the Windows dirent shim).
static int64_t level_archive_dir_size(const char* dir) {
	DIR* d = opendir(dir);
	if(!d)
		return 0;

	int64_t total = 0;
	struct dirent* e;
	while((e = readdir(d))) {
		/* P03: journals/indexes are CaveWii-only, never Beta world bytes.
		 * Filter before stat so their lifetime/cache capacity cannot affect
		 * the persisted SizeOnDisk. Still reject directories via S_ISREG. */
		size_t len = strlen(e->d_name);
		if(len < 4 || strcmp(e->d_name + len - 4, ".mcr") != 0)
			continue;
		string_t p;
		string_init_printf(p, "%s/%s", dir, e->d_name);
		struct stat st;
		unsigned long logical;
		/* G9 R19: a region with deferred writes still queued is counted at
		 * the length those writes give it -- what the synchronous path's
		 * stat would already see. */
		if(region_archive_logical_file_size(string_get_cstr(p), &logical))
			total += (int64_t)logical;
		else if(stat(string_get_cstr(p), &st) == 0 && S_ISREG(st.st_mode))
			total += (int64_t)st.st_size;
		string_clear(p);
	}
	closedir(d);
	return total;
}

/* Beta WorldInfo.sizeOnDisk is an INCREMENTAL counter: CFR 0.152 Beta
 * McRegionChunkLoader.saveChunk closes the chunk output, then adds
 * RegionFileCache.getSizeDelta -> RegionFile.func_22209_a (region allocation
 * growth, reset when read). Legacy ChunkLoader subtracts/adds file lengths.
 * World saves WorldInfo through the save handler; WorldInfo writes the long
 * as SizeOnDisk. ISaveFormat does not compute it. See docs/deviations.md.
 *
 * DOCUMENTED DEVIATION: reproducing that exact incremental algorithm here
 * would mean threading a byte-delta return value out of
 * region_archive_set_chunk/region_archive_commit_buffer -- the region file's
 * sector-allocating, gap-reusing, fault-injection-tested commit path (see
 * region_archive.c) -- through every one of its 4 call sites, into a new
 * persistent per-level running counter. That is a materially bigger, higher-
 * risk change to a hardened subsystem than this fix's scope, for a value
 * that is display-only (the Select World list's size column; it never
 * feeds worldgen/redstone/fluid/recipe/lighting or any other simulation
 * rule). Instead, this computes the WORLD'S ACTUAL CURRENT ON-DISK
 * FOOTPRINT (level.dat[+_old/_new] + region/*.mcr, both dimensions) at
 * each checkpoint -- the same player-visible contract (a real, growing
 * number instead of a frozen 0.00MB) and, unlike Beta's delta-only counter,
 * independent of CaveWii-only sidecars and region-cache capacity (P03 lead
 * ruling 2026-10-05). If a byte-exact incremental
 * tracker is wanted later, it is a dedicated follow-up against
 * region_archive.c, not a rider on this fix. */
int64_t level_archive_disk_size(struct level_archive* la) {
	assert(la && la->file_name);

	const char* path = string_get_cstr(la->file_name);
	size_t len = strlen(path);
	const char* suffix = "/level.dat";
	size_t suffix_len = strlen(suffix);
	if(len <= suffix_len || strcmp(path + len - suffix_len, suffix) != 0)
		return 0; // unexpected shape; fail closed to 0 rather than guess

	string_t world_dir;
	string_init_set_str(world_dir, path);
	string_left(world_dir, len - suffix_len);

	int64_t total = 0;
	struct stat st;
	static const char* level_files[] = {"/level.dat", "/level.dat_old",
										"/level.dat_new"};
	for(size_t k = 0; k < sizeof(level_files) / sizeof(*level_files); k++) {
		string_t p;
		string_init_printf(p, "%s%s", string_get_cstr(world_dir), level_files[k]);
		if(stat(string_get_cstr(p), &st) == 0 && S_ISREG(st.st_mode))
			total += (int64_t)st.st_size;
		string_clear(p);
	}

	string_t region_dir;
	string_init_printf(region_dir, "%s/region", string_get_cstr(world_dir));
	total += level_archive_dir_size(string_get_cstr(region_dir));
	string_clear(region_dir);

	string_t nether_region_dir;
	string_init_printf(nether_region_dir, "%s/DIM-1/region",
					   string_get_cstr(world_dir));
	total += level_archive_dir_size(string_get_cstr(nether_region_dir));
	string_clear(nether_region_dir);

	string_clear(world_dir);
	return total;
}

static bool level_archive_save_impl(struct level_archive* la) {
	assert(la && la->data);
	/* G9 R19: a synchronous save is a barrier: a queued background save of
	 * this archive lands (and is reconciled) first, so the two never reorder. */
	level_archive_settle(la, true);
	if(la->lossless_blocked && la->modified) return false;

	if(!la->modified)
		return true;
	if(!level_archive_flush(la))
		return false;

	la->modified = false;
	la->lossless_blocked = false;
	la->inventory_write_refused = false;
	return true;
}

bool level_archive_save(struct level_archive* la) {
#ifdef PLATFORM_WII
	bool profile = region_archive_work_profile_enabled();
	ptime_t start = profile ? time_get() : 0;
#endif
	bool ok = level_archive_save_impl(la);
#ifdef PLATFORM_WII
	int saved_errno = errno;
	if(profile) {
		uint32_t elapsed_us = (uint32_t)(time_diff_s(start, time_get()) * 1e6f);
		region_archive_path_profile_note(elapsed_us);
		region_archive_path_profile_emit(ok ? "level_save_end" : "level_save_fail", elapsed_us);
	}
	errno = saved_errno;
#endif
	return ok;
}

static void level_archive_destroy_impl(struct level_archive* la, bool persist) {
	assert(la && la->data);
	level_archive_settle(la, true);

	if(persist && la->modified)
		level_archive_flush(la);

	string_clear(la->file_name);
	nbt_free(la->data);
	// NULL the freed tree: a use-after-destroy (the sibling fork's exact
	// double-free crash class) becomes a deterministic assert-fire / NULL
	// bail instead of a silent dangling-pointer walk through cNBT.
	la->data = NULL;
}

void level_archive_destroy(struct level_archive* la) {
	level_archive_destroy_impl(la, true);
}

void level_archive_discard(struct level_archive* la) {
	level_archive_destroy_impl(la, false);
}
