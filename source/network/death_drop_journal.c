/*
	Copyright (c) 2026 CaveWii_v2 contributors

	See death_drop_journal.h. The file format is explicitly big-endian so a save
	created on Broadway is also readable by PC recovery/tools.
*/

/* Strict -std=c99 hides fileno in newlib/glibc unless POSIX declarations are
 * requested before any system header. Wii libfat implements fsync via f_sync,
 * so retain the durable fflush+fsync path rather than falling back to fflush. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "death_drop_journal.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../item/items.h" // direct include (was transitive via death_drop_journal.h pre-split)
#include "../item/window_container.h"
#include "../platform/mem2.h"
#include "window_registry.h"

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#define DEATH_DROP_JOURNAL_MAGIC 0x43574444u /* "CWDD" */
#define DEATH_DROP_JOURNAL_VERSION 3u
#define DEATH_DROP_JOURNAL_VERSION_LEGACY 2u
#define DEATH_DROP_JOURNAL_MAX_BYTES 8192u
#define DEATH_DROP_WORLD_COORD_LIMIT 30000000.0f
#define DEATH_DROP_VERTICAL_LIMIT 4096.0f

struct death_drop_journal* death_drop_journal_alloc(void) {
	struct death_drop_journal* journal = mem2_alloc(sizeof(*journal));
	if(journal)
		memset(journal, 0, sizeof(*journal));
	return journal;
}

void death_drop_journal_free(struct death_drop_journal* journal) {
	mem2_free_sized(journal, sizeof(*journal));
}

struct journal_writer {
	uint8_t* bytes;
	size_t capacity;
	size_t length;
};

struct journal_reader {
	const uint8_t* bytes;
	size_t length;
	size_t offset;
};

static bool put_u8(struct journal_writer* w, uint8_t value) {
	if(!w || w->length >= w->capacity)
		return false;
	w->bytes[w->length++] = value;
	return true;
}

static bool put_u16(struct journal_writer* w, uint16_t value) {
	return put_u8(w, (uint8_t)(value >> 8))
		&& put_u8(w, (uint8_t)value);
}

static bool put_u32(struct journal_writer* w, uint32_t value) {
	return put_u8(w, (uint8_t)(value >> 24))
		&& put_u8(w, (uint8_t)(value >> 16))
		&& put_u8(w, (uint8_t)(value >> 8))
		&& put_u8(w, (uint8_t)value);
}

static bool put_f32(struct journal_writer* w, float value) {
	uint32_t bits;
	if(!isfinite(value))
		return false;
	memcpy(&bits, &value, sizeof(bits));
	return put_u32(w, bits);
}

static bool get_u8(struct journal_reader* r, uint8_t* out) {
	if(!r || !out || r->offset >= r->length)
		return false;
	*out = r->bytes[r->offset++];
	return true;
}

static bool get_u16(struct journal_reader* r, uint16_t* out) {
	uint8_t a, b;
	if(!get_u8(r, &a) || !get_u8(r, &b))
		return false;
	*out = ((uint16_t)a << 8) | b;
	return true;
}

static bool get_u32(struct journal_reader* r, uint32_t* out) {
	uint8_t a, b, c, d;
	if(!get_u8(r, &a) || !get_u8(r, &b) || !get_u8(r, &c)
	   || !get_u8(r, &d))
		return false;
	*out = ((uint32_t)a << 24) | ((uint32_t)b << 16)
		| ((uint32_t)c << 8) | d;
	return true;
}

static bool get_f32(struct journal_reader* r, float* out) {
	uint32_t bits;
	if(!get_u32(r, &bits))
		return false;
	memcpy(out, &bits, sizeof(*out));
	return isfinite(*out);
}

static uint32_t journal_checksum(const uint8_t* bytes, size_t length) {
	uint32_t hash = 2166136261u;
	for(size_t i = 0; i < length; i++) {
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

static bool provenance_valid(const struct death_drop_provenance* p) {
	return p && p->kind >= DEATH_DROP_OWNER_PLAYER_SLOT
		&& p->kind <= DEATH_DROP_OWNER_CONTAINER_STORAGE
		&& (p->kind != DEATH_DROP_OWNER_CONTAINER_STORAGE
			|| (p->owner_slot >= 0 && p->owner_slot < 54));
}

static bool item_slot_valid(struct item_data item) {
	/* This legacy journal encodes only scalar fields. Refuse an ownership
	 * transfer it cannot recover losslessly, before publishing the journal. */
	if(item.nbt_record) return false;
	if(item.id == 0 || item.count == 0)
		return item.id == 0 && item.count == 0;
	/* The checksum is corruption detection, not authentication. Reject a
	 * checksum-valid crafted/bit-repaired journal before recovery can publish an
	 * unknown id that later inventory merge/render paths dereference. Preserve
	 * legacy overstacked counts here: older CaveWii saves/tests may contain them,
	 * and uint8_t ownership arithmetic remains bounded. */
	return item_get(&item) != NULL;
}

static bool container_slot_valid(const struct death_drop_container_slot* slot) {
	if(!slot || !item_slot_valid(slot->item) || slot->owner_y < 0
	   || slot->owner_y >= WORLD_HEIGHT || slot->owner_slot < 0)
		return false;
	// The per-window owner_slot bound is registry data (window_registry.h);
	// journal_slots == 0 means the window is never journaled, so a record
	// naming it is corruption. D9 shipped because WINDOWC_DISPENSER was never
	// added to the hand-maintained chain this replaces: plan_death_drops's
	// dispenser branch (D3's fix) could build a journal, but journal_valid
	// always rejected its container_slots records -- death_drop_journal_store
	// returned UNPUBLISHED_FAILURE, server_local_begin_death_drop_journal
	// returned DEATH_DROP_BEGIN_FAILED, and server_local_set_player_health
	// reverted the damage. The SAME immortality bug D3 targeted, reopened by
	// a second, deeper gap in the same feature.
	const struct window_desc* desc = window_registry_by_windowc(slot->window);
	return desc && desc->journal_slots != 0
		&& slot->owner_slot < desc->journal_slots;
}

static bool journal_valid(const struct death_drop_journal* j) {
	if(!j || (j->phase != DEATH_DROP_JOURNAL_PREPARING
			&& j->phase != DEATH_DROP_JOURNAL_PREPARED)
	   || j->transaction_id == 0
	   || (j->source_dimension != WORLD_DIM_OVERWORLD
		   && j->source_dimension != WORLD_DIM_NETHER)
	   || j->final_dimension != WORLD_DIM_OVERWORLD
	   || j->final_health <= 0
	   || j->count > DEATH_DROP_JOURNAL_MAX_RECORDS
	   || j->container_count > DEATH_DROP_JOURNAL_MAX_CONTAINER_SLOTS
	   || (j->phase == DEATH_DROP_JOURNAL_PREPARING
		   && (j->count != 0 || j->container_count != 0)))
		return false;
	for(size_t k = 0; k < 3; k++)
		if(!isfinite(j->final_position[k]))
			return false;
	if(fabsf(j->final_position[0]) > DEATH_DROP_WORLD_COORD_LIMIT
	   || fabsf(j->final_position[1]) > DEATH_DROP_VERTICAL_LIMIT
	   || fabsf(j->final_position[2]) > DEATH_DROP_WORLD_COORD_LIMIT)
		return false;
	for(size_t k = 0; k < 2; k++)
		if(!isfinite(j->final_rotation[k]))
			return false;
	if(j->final_has_player_spawn && !j->final_player_spawn_known)
		return false;
	if(j->final_has_player_spawn
	   && (j->final_player_spawn[0] < -30000000
		   || j->final_player_spawn[0] > 30000000
		   || j->final_player_spawn[2] < -30000000
		   || j->final_player_spawn[2] > 30000000
		   || j->final_player_spawn[1] < 0
		   || j->final_player_spawn[1] >= WORLD_HEIGHT))
		return false;
	for(size_t i = 0; i < j->count; i++) {
		const struct death_drop_record* r = &j->records[i];
		if(!item_slot_valid(r->item) || r->item.id == 0
		   || !provenance_valid(&r->provenance))
			return false;
		for(size_t k = 0; k < 3; k++)
			if(!isfinite(r->pos[k]) || !isfinite(r->vel[k]))
				return false;
		if(fabsf(r->pos[0]) > DEATH_DROP_WORLD_COORD_LIMIT
		   || fabsf(r->pos[1]) > DEATH_DROP_VERTICAL_LIMIT
		   || fabsf(r->pos[2]) > DEATH_DROP_WORLD_COORD_LIMIT
		   || fabsf(r->vel[0]) > 10000.0f
		   || fabsf(r->vel[1]) > 10000.0f
		   || fabsf(r->vel[2]) > 10000.0f)
			return false;
	}
	for(size_t i = 0; i < j->container_count; i++) {
		const struct death_drop_container_slot* slot
			= &j->container_slots[i];
		if(!container_slot_valid(slot))
			return false;
		if(slot->owner_x < -(int32_t)DEATH_DROP_WORLD_COORD_LIMIT
		   || slot->owner_x > (int32_t)DEATH_DROP_WORLD_COORD_LIMIT
		   || slot->owner_z < -(int32_t)DEATH_DROP_WORLD_COORD_LIMIT
		   || slot->owner_z > (int32_t)DEATH_DROP_WORLD_COORD_LIMIT)
			return false;
		for(size_t k = 0; k < i; k++) {
			const struct death_drop_container_slot* prior
				= &j->container_slots[k];
			if(prior->owner_x == slot->owner_x
			   && prior->owner_y == slot->owner_y
			   && prior->owner_z == slot->owner_z
			   && prior->owner_slot == slot->owner_slot)
				return false;
		}
	}
	return true;
}

static bool journal_encode(const struct death_drop_journal* j,
						   uint8_t* bytes, size_t capacity, size_t* length_out) {
	if(!journal_valid(j) || !bytes || !length_out)
		return false;
	struct journal_writer w = {bytes, capacity, 0};
	if(!put_u32(&w, DEATH_DROP_JOURNAL_MAGIC)
	   || !put_u16(&w, DEATH_DROP_JOURNAL_VERSION)
	   || !put_u8(&w, (uint8_t)j->phase)
	   || !put_u8(&w, (uint8_t)j->source_dimension)
	   || !put_u32(&w, j->transaction_id)
	   || !put_u16(&w, (uint16_t)j->count)
	   || !put_u16(&w, (uint16_t)j->container_count))
		return false;
	for(size_t k = 0; k < 3; k++)
		if(!put_f32(&w, j->final_position[k]))
			return false;
	for(size_t k = 0; k < 2; k++)
		if(!put_f32(&w, j->final_rotation[k]))
			return false;
	if(!put_u8(&w, (uint8_t)j->final_dimension)
	   || !put_u16(&w, (uint16_t)j->final_health))
		return false;
	uint8_t spawn_state = !j->final_player_spawn_known ? 2u
		: j->final_has_player_spawn ? 1u : 0u;
	if(!put_u8(&w, spawn_state))
		return false;
	for(size_t k = 0; k < 3; k++)
		if(!put_u32(&w, (uint32_t)j->final_player_spawn[k]))
			return false;

	for(size_t i = 0; i < j->count; i++) {
		const struct death_drop_record* r = &j->records[i];
		if(!put_u16(&w, r->item.id) || !put_u8(&w, r->item.count)
		   || !put_u16(&w, r->item.durability))
			return false;
		for(size_t k = 0; k < 3; k++)
			if(!put_f32(&w, r->pos[k]))
				return false;
		for(size_t k = 0; k < 3; k++)
			if(!put_f32(&w, r->vel[k]))
				return false;
		if(!put_u8(&w, r->provenance.kind)
		   || !put_u8(&w, r->provenance.window)
		   || !put_u16(&w, (uint16_t)r->provenance.slot)
		   || !put_u16(&w, (uint16_t)r->provenance.owner_slot)
		   || !put_u32(&w, (uint32_t)r->provenance.owner_x)
		   || !put_u32(&w, (uint32_t)r->provenance.owner_y)
		   || !put_u32(&w, (uint32_t)r->provenance.owner_z))
			return false;
	}
	for(size_t i = 0; i < j->container_count; i++) {
		const struct death_drop_container_slot* slot
			= &j->container_slots[i];
		if(!put_u16(&w, slot->item.id)
		   || !put_u8(&w, slot->item.count)
		   || !put_u16(&w, slot->item.durability)
		   || !put_u8(&w, slot->window)
		   || !put_u16(&w, (uint16_t)slot->owner_slot)
		   || !put_u32(&w, (uint32_t)slot->owner_x)
		   || !put_u32(&w, (uint32_t)slot->owner_y)
		   || !put_u32(&w, (uint32_t)slot->owner_z))
			return false;
	}
	uint32_t checksum = journal_checksum(bytes, w.length);
	if(!put_u32(&w, checksum))
		return false;
	*length_out = w.length;
	return true;
}

static bool journal_decode(const uint8_t* bytes, size_t length,
						   struct death_drop_journal* j) {
	if(!bytes || !j || length < 4)
		return false;
	uint32_t expected = ((uint32_t)bytes[length - 4] << 24)
		| ((uint32_t)bytes[length - 3] << 16)
		| ((uint32_t)bytes[length - 2] << 8) | bytes[length - 1];
	if(journal_checksum(bytes, length - 4) != expected)
		return false;

	memset(j, 0, sizeof(*j));
	struct journal_reader r = {bytes, length - 4, 0};
	uint32_t magic, tx;
	uint16_t version, count, container_count, health;
	uint8_t phase, source_dimension, final_dimension;
	if(!get_u32(&r, &magic) || !get_u16(&r, &version)
	   || !get_u8(&r, &phase) || !get_u8(&r, &source_dimension)
	   || !get_u32(&r, &tx) || !get_u16(&r, &count)
	   || !get_u16(&r, &container_count)
	   || magic != DEATH_DROP_JOURNAL_MAGIC
	   || (version != DEATH_DROP_JOURNAL_VERSION
		   && version != DEATH_DROP_JOURNAL_VERSION_LEGACY)
	   || count > DEATH_DROP_JOURNAL_MAX_RECORDS
	   || container_count > DEATH_DROP_JOURNAL_MAX_CONTAINER_SLOTS)
		return false;
	j->phase = (enum death_drop_journal_phase)phase;
	j->source_dimension = (enum world_dim)(int8_t)source_dimension;
	j->transaction_id = tx;
	j->count = count;
	j->container_count = container_count;
	for(size_t k = 0; k < 3; k++)
		if(!get_f32(&r, &j->final_position[k]))
			return false;
	for(size_t k = 0; k < 2; k++)
		if(!get_f32(&r, &j->final_rotation[k]))
			return false;
	if(!get_u8(&r, &final_dimension) || !get_u16(&r, &health))
		return false;
	j->final_dimension = (enum world_dim)(int8_t)final_dimension;
	j->final_health = (int16_t)health;
	if(version >= DEATH_DROP_JOURNAL_VERSION) {
		uint8_t spawn_state;
		uint32_t spawn[3];
		if(!get_u8(&r, &spawn_state) || spawn_state > 2)
			return false;
		for(size_t k = 0; k < 3; k++)
			if(!get_u32(&r, &spawn[k]))
				return false;
		j->final_player_spawn_known = spawn_state != 2;
		j->final_has_player_spawn = spawn_state == 1;
		for(size_t k = 0; k < 3; k++)
			j->final_player_spawn[k] = (int32_t)spawn[k];
	}

	for(size_t i = 0; i < j->count; i++) {
		struct death_drop_record* rec = &j->records[i];
		uint16_t id, durability, slot, owner_slot;
		uint8_t item_count, kind, window;
		uint32_t owner_x, owner_y, owner_z;
		if(!get_u16(&r, &id) || !get_u8(&r, &item_count)
		   || !get_u16(&r, &durability))
			return false;
		rec->item = (struct item_data) {
			.id = id, .count = item_count, .durability = durability,
		};
		for(size_t k = 0; k < 3; k++)
			if(!get_f32(&r, &rec->pos[k]))
				return false;
		for(size_t k = 0; k < 3; k++)
			if(!get_f32(&r, &rec->vel[k]))
				return false;
		if(!get_u8(&r, &kind) || !get_u8(&r, &window)
		   || !get_u16(&r, &slot) || !get_u16(&r, &owner_slot)
		   || !get_u32(&r, &owner_x)
		   || !get_u32(&r, &owner_y) || !get_u32(&r, &owner_z))
			return false;
		rec->provenance = (struct death_drop_provenance) {
			.kind = kind,
			.window = window,
			.slot = (int16_t)slot,
			.owner_slot = (int16_t)owner_slot,
			.owner_x = (int32_t)owner_x,
			.owner_y = (int32_t)owner_y,
			.owner_z = (int32_t)owner_z,
		};
	}
	for(size_t i = 0; i < j->container_count; i++) {
		struct death_drop_container_slot* slot = &j->container_slots[i];
		uint16_t id, durability, owner_slot;
		uint8_t item_count, window;
		uint32_t owner_x, owner_y, owner_z;
		if(!get_u16(&r, &id) || !get_u8(&r, &item_count)
		   || !get_u16(&r, &durability) || !get_u8(&r, &window)
		   || !get_u16(&r, &owner_slot) || !get_u32(&r, &owner_x)
		   || !get_u32(&r, &owner_y) || !get_u32(&r, &owner_z))
			return false;
		slot->item = (struct item_data) {
			.id = id, .count = item_count, .durability = durability,
		};
		slot->window = window;
		slot->owner_slot = (int16_t)owner_slot;
		slot->owner_x = (int32_t)owner_x;
		slot->owner_y = (int32_t)owner_y;
		slot->owner_z = (int32_t)owner_z;
	}
	return r.offset == r.length && journal_valid(j);
}

static bool file_sync(FILE* f) {
	if(!f || fflush(f) != 0)
		return false;
#ifdef _WIN32
	return _commit(_fileno(f)) == 0;
#else
	return fsync(fileno(f)) == 0;
#endif
}

#ifdef CAVEWII_DEATH_DROP_TEST
static unsigned s_fail_countdown[DEATH_DROP_JOURNAL_TEST_FAIL_COUNT];

void death_drop_journal_test_fail_next(
	enum death_drop_journal_test_fail_stage stage) {
	death_drop_journal_test_fail_after(stage, 1);
}

void death_drop_journal_test_fail_after(
	enum death_drop_journal_test_fail_stage stage, unsigned occurrence) {
	assert(stage > DEATH_DROP_JOURNAL_TEST_FAIL_NONE
		   && stage < DEATH_DROP_JOURNAL_TEST_FAIL_COUNT);
	s_fail_countdown[stage] = occurrence;
}

static bool should_fail(enum death_drop_journal_test_fail_stage stage) {
	assert(stage > DEATH_DROP_JOURNAL_TEST_FAIL_NONE
		   && stage < DEATH_DROP_JOURNAL_TEST_FAIL_COUNT);
	if(s_fail_countdown[stage] == 0 || --s_fail_countdown[stage] != 0)
		return false;
	return true;
}
#endif

static void journal_paths(string_t world_name, string_t current,
						  string_t fresh, string_t old) {
	string_init_printf(current, "%s/death_drops.dat",
					   string_get_cstr(world_name));
	string_init_printf(fresh, "%s_new", string_get_cstr(current));
	string_init_printf(old, "%s_old", string_get_cstr(current));
}

enum death_drop_journal_store_result death_drop_journal_store(
	string_t world_name, const struct death_drop_journal* journal) {
	uint8_t bytes[DEATH_DROP_JOURNAL_MAX_BYTES];
	size_t length;
	if(!world_name || !journal_encode(
					   journal, bytes, sizeof(bytes), &length))
		return DEATH_DROP_JOURNAL_STORE_UNPUBLISHED_FAILURE;
	string_t current, fresh, old;
	journal_paths(world_name, current, fresh, old);
	bool fresh_ok = false;
	FILE* f = fopen(string_get_cstr(fresh), "wb");
	if(f) {
#ifdef CAVEWII_DEATH_DROP_TEST
		if(should_fail(DEATH_DROP_JOURNAL_TEST_FAIL_WRITE))
			fresh_ok = false;
		else
#endif
			fresh_ok = fwrite(bytes, length, 1, f) == 1 && file_sync(f);
		if(fclose(f) != 0)
			fresh_ok = false;
	}
	enum death_drop_journal_store_result result
		= DEATH_DROP_JOURNAL_STORE_UNPUBLISHED_FAILURE;
	if(fresh_ok) {
		remove(string_get_cstr(old));
		bool had_current
			= rename(string_get_cstr(current), string_get_cstr(old)) == 0;
		bool published;
#ifdef CAVEWII_DEATH_DROP_TEST
		if(should_fail(DEATH_DROP_JOURNAL_TEST_FAIL_RENAME))
			published = false;
		else
#endif
			published
				= rename(string_get_cstr(fresh), string_get_cstr(current)) == 0;
		if(!published && had_current)
			rename(string_get_cstr(old), string_get_cstr(current));
		if(published) {
			result = DEATH_DROP_JOURNAL_STORE_VALID_PUBLISHED_UNSYNCED;
			/* Sync the committed name too. A successful rename makes the valid
			 * journal observable, but only this second sync proves the decision
			 * durable enough for a caller to clear a source slot. */
			bool committed_ok = false;
#ifdef CAVEWII_DEATH_DROP_TEST
			if(!should_fail(DEATH_DROP_JOURNAL_TEST_FAIL_COMMITTED_SYNC))
#endif
			{
				FILE* committed = fopen(string_get_cstr(current), "rb+");
				committed_ok = committed && file_sync(committed);
				if(committed && fclose(committed) != 0)
					committed_ok = false;
			}
			if(committed_ok)
				result = DEATH_DROP_JOURNAL_STORE_DURABLE;
		}
	}
	if(result == DEATH_DROP_JOURNAL_STORE_UNPUBLISHED_FAILURE)
		remove(string_get_cstr(fresh));
	string_clear(current);
	string_clear(fresh);
	string_clear(old);
	return result;
}

static bool journal_read_file(const char* path,
						  struct death_drop_journal* journal) {
	FILE* f = fopen(path, "rb");
	if(!f)
		return false;
	uint8_t bytes[DEATH_DROP_JOURNAL_MAX_BYTES];
	size_t length = fread(bytes, 1, sizeof(bytes), f);
	bool whole = !ferror(f) && fgetc(f) == EOF;
	fclose(f);
	return whole && journal_decode(bytes, length, journal);
}

enum death_drop_journal_load_result death_drop_journal_load(
	string_t world_name, struct death_drop_journal* journal) {
	if(!world_name || !journal)
		return DEATH_DROP_JOURNAL_LOAD_ERROR;
	string_t current, fresh, old;
	journal_paths(world_name, current, fresh, old);
	if(journal_read_file(string_get_cstr(current), journal)) {
		string_clear(current);
		string_clear(fresh);
		string_clear(old);
		return DEATH_DROP_JOURNAL_LOAD_OK;
	}
	bool current_exists = false;
	FILE* current_file = fopen(string_get_cstr(current), "rb");
	if(current_file) {
		current_exists = true;
		fclose(current_file);
	}
	if(current_exists) {
		/* A corrupt committed journal is not an absent transaction. Never guess
		 * between keeping inventory and committing drops. */
		string_clear(current);
		string_clear(fresh);
		string_clear(old);
		return DEATH_DROP_JOURNAL_LOAD_ERROR;
	}

	const char* survivor = NULL;
	if(journal_read_file(string_get_cstr(fresh), journal))
		survivor = string_get_cstr(fresh);
	else if(journal_read_file(string_get_cstr(old), journal))
		survivor = string_get_cstr(old);
	if(survivor) {
		if(rename(survivor, string_get_cstr(current)) != 0) {
			string_clear(current);
			string_clear(fresh);
			string_clear(old);
			return DEATH_DROP_JOURNAL_LOAD_ERROR;
		}
		/* Match store(): on libfat fsyncing the reopened committed file flushes
		 * both file data and the volume metadata dirtied by rename. */
		FILE* committed = fopen(string_get_cstr(current), "rb+");
		bool committed_ok = committed && file_sync(committed);
		if(committed && fclose(committed) != 0)
			committed_ok = false;
		if(!committed_ok) {
			string_clear(current);
			string_clear(fresh);
			string_clear(old);
			return DEATH_DROP_JOURNAL_LOAD_ERROR;
		}
		string_clear(current);
		string_clear(fresh);
		string_clear(old);
		return DEATH_DROP_JOURNAL_LOAD_OK;
	}

	bool survivor_exists = false;
	FILE* f = fopen(string_get_cstr(fresh), "rb");
	if(f) {
		survivor_exists = true;
		fclose(f);
	}
	f = fopen(string_get_cstr(old), "rb");
	if(f) {
		survivor_exists = true;
		fclose(f);
	}
	string_clear(current);
	string_clear(fresh);
	string_clear(old);
	return survivor_exists ? DEATH_DROP_JOURNAL_LOAD_ERROR
						   : DEATH_DROP_JOURNAL_LOAD_NONE;
}

static bool remove_if_present(const char* path) {
#ifdef CAVEWII_DEATH_DROP_TEST
	if(should_fail(DEATH_DROP_JOURNAL_TEST_FAIL_REMOVE_PATH))
		return false;
#endif
	if(remove(path) == 0)
		return true;
	return errno == ENOENT;
}

bool death_drop_journal_remove(string_t world_name) {
	if(!world_name)
		return false;
#ifdef CAVEWII_DEATH_DROP_TEST
	if(should_fail(DEATH_DROP_JOURNAL_TEST_FAIL_REMOVE))
		return false;
#endif
	string_t current, fresh, old;
	journal_paths(world_name, current, fresh, old);
	/* Remove recovery candidates before the authoritative name. If cleanup is
	 * interrupted, current must remain loadable instead of exposing a stale or
	 * amended sidecar as a new transaction on restart. */
	bool ok = remove_if_present(string_get_cstr(fresh));
	if(ok)
		ok = remove_if_present(string_get_cstr(old));
	if(ok)
		ok = remove_if_present(string_get_cstr(current));
	string_clear(current);
	string_clear(fresh);
	string_clear(old);
	return ok;
}
