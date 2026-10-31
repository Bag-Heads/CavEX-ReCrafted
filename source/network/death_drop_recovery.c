/*
	Copyright (c) 2026 CaveWii_v2 contributors
*/

#include "death_drop_recovery.h"
#include "server_local_state.h" // full server state (split from server_local.h) -- MAX_CHESTS/MAX_CHEST_SLOTS/MAX_SIGNS, struct complex_block_pos by value

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "death_drop_journal.h"
#include "complex_block_archive.h"
#include "entity_archive.h"
#include "level_archive.h"
#include "../platform/mem2.h"
#include "../item/window_container.h"
#include "region_archive.h"
#include "server_world.h"
#include "tile_entities_nbt.h"
#include "window_registry.h"

/* Which tile entity a persisted window byte resolves to is registry data
 * (window_registry.h). Adding a kind without giving it an arm in all three
 * functions below is how crash replay HARD-FAILS world recovery on an
 * unrecognised window (D10); this stops the build instead. */
_Static_assert(WINDOW_RECOVERY_COUNT == 4,
			   "new enum window_recovery kind: add its arm to "
			   "clear_container_sources, clear_legacy_container_sources and "
			   "apply_container_slots (CODESTYLE.md S3.3)");

static enum window_recovery recovery_kind(uint8_t window) {
	const struct window_desc* desc = window_registry_by_windowc(window);
	return desc ? desc->recovery : WINDOW_RECOVERY_NONE;
}

struct recovery_chunk {
	w_coord_t x;
	w_coord_t z;
};

#ifdef CAVEWII_DEATH_DROP_TEST
static bool s_fail_next_alloc;
static unsigned s_fail_after_chunks;

void death_drop_recovery_test_fail_next_alloc(void) {
	s_fail_next_alloc = true;
}

void death_drop_recovery_test_fail_after_chunks(unsigned count) {
	s_fail_after_chunks = count;
}

static bool recovery_alloc_should_fail(void) {
	if(!s_fail_next_alloc)
		return false;
	s_fail_next_alloc = false;
	return true;
}
#endif

static w_coord_t record_chunk(float coordinate) {
	return (w_coord_t)floorf(coordinate / (float)CHUNK_SIZE);
}

static bool record_in_chunk(const struct death_drop_record* record,
							w_coord_t x, w_coord_t z) {
	return record && record_chunk(record->pos[0]) == x
		&& record_chunk(record->pos[2]) == z;
}

static size_t collect_chunks(const struct death_drop_journal* journal,
							 struct recovery_chunk* chunks) {
	size_t count = 0;
	for(size_t i = 0; i < journal->count; i++) {
		struct recovery_chunk candidates[2] = {{
			.x = record_chunk(journal->records[i].pos[0]),
			.z = record_chunk(journal->records[i].pos[2]),
		}};
		int candidate_count = 1;
		if(journal->records[i].provenance.kind
		   == DEATH_DROP_OWNER_CONTAINER_STORAGE) {
			candidates[1].x = (w_coord_t)floor(
				(double)journal->records[i].provenance.owner_x / CHUNK_SIZE);
			candidates[1].z = (w_coord_t)floor(
				(double)journal->records[i].provenance.owner_z / CHUNK_SIZE);
			candidate_count = 2;
		}
		for(int c = 0; c < candidate_count; c++) {
			bool seen = false;
			for(size_t k = 0; k < count; k++)
				if(chunks[k].x == candidates[c].x
				   && chunks[k].z == candidates[c].z) {
					seen = true;
					break;
				}
			if(!seen)
				chunks[count++] = candidates[c];
		}
	}
	for(size_t i = 0; i < journal->container_count; i++) {
		const struct death_drop_container_slot* slot
			= &journal->container_slots[i];
		struct recovery_chunk candidate = {
			.x = (w_coord_t)floor((double)slot->owner_x / CHUNK_SIZE),
			.z = (w_coord_t)floor((double)slot->owner_z / CHUNK_SIZE),
		};
		bool seen = false;
		for(size_t k = 0; k < count; k++)
			if(chunks[k].x == candidate.x && chunks[k].z == candidate.z) {
				seen = true;
				break;
			}
		if(!seen)
			chunks[count++] = candidate;
	}
	return count;
}

static bool item_equal(struct item_data a, struct item_data b) {
	return a.id == b.id && a.count == b.count
		&& a.durability == b.durability;
}

static bool region_path_status(string_t world_name,
							   enum world_dim dimension, w_coord_t chunk_x,
							   w_coord_t chunk_z, bool* exists) {
	string_t path;
	if(dimension == WORLD_DIM_NETHER)
		string_init_printf(path, "%s/DIM-1/region/r.%i.%i.mcr",
						   string_get_cstr(world_name),
						   CHUNK_REGION_COORD(chunk_x),
						   CHUNK_REGION_COORD(chunk_z));
	else
		string_init_printf(path, "%s/region/r.%i.%i.mcr",
						   string_get_cstr(world_name),
						   CHUNK_REGION_COORD(chunk_x),
						   CHUNK_REGION_COORD(chunk_z));
	struct stat st;
	errno = 0;
	int result = stat(string_get_cstr(path), &st);
	int error = errno;
	string_clear(path);
	if(result == 0) {
		*exists = true;
		return true;
	}
	*exists = false;
	return error == ENOENT;
}

static void archived_from_record(struct archived_entity* entity,
								 const struct death_drop_journal* journal,
								 size_t sequence) {
	const struct death_drop_record* record = &journal->records[sequence];
	memset(entity, 0, sizeof(*entity));
	entity->kind = ARCHIVED_ENTITY_ITEM;
	memcpy(entity->pos, record->pos, sizeof(entity->pos));
	memcpy(entity->vel, record->vel, sizeof(entity->vel));
	entity->data.item.item = record->item;
	entity->data.item.death_transaction = journal->transaction_id;
	entity->data.item.death_sequence = (uint16_t)sequence;
}

static bool clear_container_sources(
	struct chunk_tile_entities* tile_entities,
	const struct death_drop_journal* journal, w_coord_t chunk_x,
	w_coord_t chunk_z, bool* changed) {
	for(size_t i = 0; i < journal->count; i++) {
		const struct death_drop_provenance* p
			= &journal->records[i].provenance;
		if(p->kind != DEATH_DROP_OWNER_CONTAINER_STORAGE
		   || (w_coord_t)floor((double)p->owner_x / CHUNK_SIZE) != chunk_x
		   || (w_coord_t)floor((double)p->owner_z / CHUNK_SIZE) != chunk_z
		   || p->owner_slot < 0)
			continue;
		if(recovery_kind(p->window) == WINDOW_RECOVERY_TE_FURNACE) {
			if(p->owner_slot >= TE_FURNACE_SLOTS)
				return false;
			for(int k = 0; k < tile_entities->n_furnace; k++) {
				struct te_furnace* furnace = &tile_entities->furnaces[k];
				if(furnace->x == p->owner_x && furnace->y == p->owner_y
				   && furnace->z == p->owner_z
				   && furnace->items[p->owner_slot].id != 0) {
					furnace->items[p->owner_slot] = (struct item_data) {0};
					*changed = true;
				}
			}
		} else if(recovery_kind(p->window) == WINDOW_RECOVERY_TE_DISPENSER) {
			/* D10: a dispenser used to land in the chest arm below, find no
			 * chest at its position, and silently leave its transferred slots
			 * in place -- the drop record and the tile entity would both own
			 * the same stack after recovery. */
			if(p->owner_slot >= TE_DISPENSER_SLOTS)
				return false;
			for(int k = 0; k < tile_entities->n_dispenser; k++) {
				struct te_dispenser* dispenser = &tile_entities->dispensers[k];
				if(dispenser->x == p->owner_x && dispenser->y == p->owner_y
				   && dispenser->z == p->owner_z
				   && dispenser->items[p->owner_slot].id != 0) {
					dispenser->items[p->owner_slot] = (struct item_data) {0};
					*changed = true;
				}
			}
		} else {
			if(p->owner_slot >= TE_CHEST_SLOTS)
				return false;
			for(int k = 0; k < tile_entities->n_chest; k++) {
				struct te_chest* chest = &tile_entities->chests[k];
				if(chest->x == p->owner_x && chest->y == p->owner_y
				   && chest->z == p->owner_z
				   && chest->items[p->owner_slot].id != 0) {
					chest->items[p->owner_slot] = (struct item_data) {0};
					*changed = true;
				}
			}
		}
	}
	return true;
}

/* Raw recovery may be rewriting an old, unmarked chunk whose TileEntities list
 * never contained legacy-only chests.dat rows. It must not mark that incomplete
 * list authoritative. Instead, while PREPARED remains durable, clear only the
 * exact transferred chest slots in the flat archive. A crash/failure leaves the
 * journal to retry; unrelated rows, including rows in the same chunk, survive. */
static bool clear_legacy_container_sources(
	string_t world_name, const struct death_drop_journal* journal) {
	bool has_legacy_source = false;
	for(size_t i = 0; i < journal->count; i++) {
		const struct death_drop_provenance* p
			= &journal->records[i].provenance;
		/* D10: only the CHEST kind ever lived in the legacy flat chests.dat.
		 * A journaled dispenser used to test as "not a furnace" and so drag
		 * this whole read/rewrite of chests.dat in behind it, matching no row
		 * and changing nothing. Testing for the chest kind positively also
		 * keeps a corrupt window byte (recovery kind NONE) out of here, which
		 * is what apply_container_slots below already does. */
		if(p->kind == DEATH_DROP_OWNER_CONTAINER_STORAGE
		   && recovery_kind(p->window) == WINDOW_RECOVERY_TE_CHEST) {
			has_legacy_source = true;
			break;
		}
	}
	for(size_t i = 0; !has_legacy_source && i < journal->container_count; i++)
		if(recovery_kind(journal->container_slots[i].window)
		   == WINDOW_RECOVERY_TE_CHEST)
			has_legacy_source = true;
	if(!has_legacy_source)
		return true;

	const size_t pos_bytes
		= (size_t)MAX_CHESTS * sizeof(struct complex_block_pos);
	const size_t item_bytes = (size_t)MAX_CHESTS * MAX_CHEST_SLOTS
		* sizeof(struct item_data);
	struct complex_block_pos* positions = mem2_alloc(pos_bytes);
	struct item_data* items = mem2_alloc(item_bytes);
	if(!positions || !items) {
		mem2_free_sized(positions, pos_bytes);
		mem2_free_sized(items, item_bytes);
		return false;
	}
	if(!chest_archive_read(
		   positions, items, world_name, journal->source_dimension)) {
		mem2_free_sized(positions, pos_bytes);
		mem2_free_sized(items, item_bytes);
		return false;
	}
	for(size_t i = 0; i < journal->count; i++) {
		const struct death_drop_provenance* p
			= &journal->records[i].provenance;
		if(p->kind != DEATH_DROP_OWNER_CONTAINER_STORAGE
		   || recovery_kind(p->window) != WINDOW_RECOVERY_TE_CHEST
		   || p->owner_slot < 0 || p->owner_slot >= MAX_CHEST_SLOTS)
			continue;
		for(int row = 0; row < MAX_CHESTS; row++)
			if(positions[row].x == p->owner_x
			   && positions[row].y == p->owner_y
			   && positions[row].z == p->owner_z) {
				items[(size_t)row * MAX_CHEST_SLOTS
					  + (size_t)p->owner_slot]
					= (struct item_data) {0};
				break;
			}
	}
	/* A valid open chest records its complete post-close storage image,
	 * including empty slots. Apply it after drop-source clears so the exact
	 * post-close owner wins if both provenance forms mention one slot. */
	for(size_t i = 0; i < journal->container_count; i++) {
		const struct death_drop_container_slot* slot
			= &journal->container_slots[i];
		if(recovery_kind(slot->window) != WINDOW_RECOVERY_TE_CHEST
		   || slot->owner_slot < 0 || slot->owner_slot >= MAX_CHEST_SLOTS)
			continue;
		for(int row = 0; row < MAX_CHESTS; row++)
			if(positions[row].x == slot->owner_x
			   && positions[row].y == slot->owner_y
			   && positions[row].z == slot->owner_z) {
				items[(size_t)row * MAX_CHEST_SLOTS
					  + (size_t)slot->owner_slot]
					= slot->item;
				break;
			}
	}
	bool ok = chest_archive_write(
		positions, items, world_name, journal->source_dimension);
	mem2_free_sized(positions, pos_bytes);
	mem2_free_sized(items, item_bytes);
	return ok;
}

static bool apply_container_slots(
	struct chunk_tile_entities* tile_entities,
	const struct death_drop_journal* journal, w_coord_t chunk_x,
	w_coord_t chunk_z, bool* changed) {
	for(size_t i = 0; i < journal->container_count; i++) {
		const struct death_drop_container_slot* slot
			= &journal->container_slots[i];
		if((w_coord_t)floor((double)slot->owner_x / CHUNK_SIZE) != chunk_x
		   || (w_coord_t)floor((double)slot->owner_z / CHUNK_SIZE) != chunk_z)
			continue;

		bool found = false;
		if(recovery_kind(slot->window) == WINDOW_RECOVERY_TE_FURNACE) {
			if(slot->owner_slot < 0 || slot->owner_slot >= TE_FURNACE_SLOTS)
				return false;
			for(int k = 0; k < tile_entities->n_furnace; k++) {
				struct te_furnace* furnace = &tile_entities->furnaces[k];
				if(furnace->x != slot->owner_x
				   || furnace->y != slot->owner_y
				   || furnace->z != slot->owner_z)
					continue;
				found = true;
				if(!item_equal(
					   furnace->items[slot->owner_slot], slot->item)) {
					furnace->items[slot->owner_slot] = slot->item;
					*changed = true;
				}
				break;
			}
		} else if(recovery_kind(slot->window)
				  == WINDOW_RECOVERY_TE_DISPENSER) {
			/* D10: a journaled dispenser used to reach the chest arm below,
			 * fail its window check and return false -- crash replay
			 * HARD-FAILED world recovery for every save whose owner died with
			 * a dispenser GUI open. */
			if(slot->owner_slot < 0 || slot->owner_slot >= TE_DISPENSER_SLOTS)
				return false;
			for(int k = 0; k < tile_entities->n_dispenser; k++) {
				struct te_dispenser* dispenser = &tile_entities->dispensers[k];
				if(dispenser->x != slot->owner_x
				   || dispenser->y != slot->owner_y
				   || dispenser->z != slot->owner_z)
					continue;
				found = true;
				if(!item_equal(
					   dispenser->items[slot->owner_slot], slot->item)) {
					dispenser->items[slot->owner_slot] = slot->item;
					*changed = true;
				}
				break;
			}
		} else {
			if(recovery_kind(slot->window) != WINDOW_RECOVERY_TE_CHEST
			   || slot->owner_slot < 0
			   || slot->owner_slot >= TE_CHEST_SLOTS)
				return false;
			for(int k = 0; k < tile_entities->n_chest; k++) {
				struct te_chest* chest = &tile_entities->chests[k];
				if(chest->x != slot->owner_x || chest->y != slot->owner_y
				   || chest->z != slot->owner_z)
					continue;
				found = true;
				if(!item_equal(chest->items[slot->owner_slot], slot->item)) {
					chest->items[slot->owner_slot] = slot->item;
					*changed = true;
				}
				break;
			}
		}
		/* PREPARED was written only after the live backing owner was
		 * checkpointed. Missing it now is corruption, not an empty container. */
		if(!found)
			return false;
	}
	return true;
}

static void recovery_chunk_destroy(struct server_chunk* chunk) {
	if(!chunk)
		return;
	/* One MEM2_POOL_COLUMN slot on the Wii, five arrays on PC (Astra P1,
	 * 2026-09-11): the helper knows which shape this chunk has. */
	server_chunk_arrays_free(chunk);
}

static bool recover_chunk(string_t world_name,
						  const struct death_drop_journal* journal,
						  w_coord_t chunk_x, w_coord_t chunk_z,
						  bool complete_forward) {
	bool file_exists;
	if(!region_path_status(world_name, journal->source_dimension, chunk_x,
						   chunk_z, &file_exists))
		return false;
	if(!file_exists)
		return !complete_forward; /* PREPARED preflight guarantees a base chunk. */

	/* A SECOND HANDLE on a region the world LRU may already hold. Recovery runs
	 * from server_local_load_world_impl_ex BEFORE server_world_create, so in the
	 * current call graph the LRU is empty -- but that is an ordering the caller
	 * owns, not an invariant this function can enforce. The record it writes
	 * below is never canonical, so it CLEARS the per-chunk canonical flag on
	 * disk; a cached handle whose in-memory `ra->canon` still had that bit set
	 * would go on skipping the pre-save read-back and silently drop the very
	 * entities this recovery just restored. region_archive.c's open-region table
	 * closes that: any overlapping open on the same path permanently disarms the
	 * canonical fast path for every handle involved (see THE SECOND-HANDLE GUARD
	 * there, and check_canonical_second_handle). */
	struct region_archive region;
	if(!region_archive_create(
		   &region, world_name, CHUNK_REGION_COORD(chunk_x),
		   CHUNK_REGION_COORD(chunk_z), journal->source_dimension))
		return false;
	bool chunk_exists = false;
	if(!region_archive_contains(&region, chunk_x, chunk_z, &chunk_exists)
	   || (!chunk_exists && complete_forward)) {
		region_archive_destroy(&region);
		return !complete_forward && !chunk_exists;
	}
	if(!chunk_exists) {
		region_archive_destroy(&region);
		return true;
	}

	struct server_chunk chunk = {0};
	struct chunk_tile_entities tile_entities = {0};
	struct chunk_entities old_entities = {0};
	if(!region_archive_get_chunk(&region, chunk_x, chunk_z, &chunk,
								 &tile_entities, &old_entities)) {
		region_archive_destroy(&region);
		return false;
	}

	if(old_entities.count > SIZE_MAX - journal->count) {
		tile_entities_parse_free(&tile_entities);
		chunk_entities_free(&old_entities);
		recovery_chunk_destroy(&chunk);
		region_archive_destroy(&region);
		return false;
	}
	size_t capacity = old_entities.count + journal->count;
	struct archived_entity* rebuilt = NULL;
#ifdef CAVEWII_DEATH_DROP_TEST
	if(capacity > 0 && recovery_alloc_should_fail()) {
		tile_entities_parse_free(&tile_entities);
		chunk_entities_free(&old_entities);
		recovery_chunk_destroy(&chunk);
		region_archive_destroy(&region);
		return false;
	}
#endif
	if(capacity > 0)
		rebuilt = calloc(capacity, sizeof(*rebuilt));
	if(capacity > 0 && !rebuilt) {
		tile_entities_parse_free(&tile_entities);
		chunk_entities_free(&old_entities);
		recovery_chunk_destroy(&chunk);
		region_archive_destroy(&region);
		return false;
	}

	bool seen[DEATH_DROP_JOURNAL_MAX_RECORDS] = {false};
	size_t rebuilt_count = 0;
	bool changed = false;
	if(complete_forward
	   && !clear_container_sources(
		   &tile_entities, journal, chunk_x, chunk_z, &changed)) {
		free(rebuilt);
		tile_entities_parse_free(&tile_entities);
		chunk_entities_free(&old_entities);
		recovery_chunk_destroy(&chunk);
		region_archive_destroy(&region);
		return false;
	}
	if(complete_forward
	   && !apply_container_slots(
		   &tile_entities, journal, chunk_x, chunk_z, &changed)) {
		free(rebuilt);
		tile_entities_parse_free(&tile_entities);
		chunk_entities_free(&old_entities);
		recovery_chunk_destroy(&chunk);
		region_archive_destroy(&region);
		return false;
	}
	for(size_t i = 0; i < old_entities.count; i++) {
		const struct archived_entity* entity = &old_entities.entities[i];
		if(entity->kind != ARCHIVED_ENTITY_ITEM
		   || entity->data.item.death_transaction
			   != journal->transaction_id) {
			rebuilt[rebuilt_count++] = *entity;
			continue;
		}

		size_t sequence = entity->data.item.death_sequence;
		bool canonical = complete_forward && sequence < journal->count
			&& record_in_chunk(&journal->records[sequence], chunk_x, chunk_z)
			&& item_equal(entity->data.item.item,
						  journal->records[sequence].item)
			&& !seen[sequence];
		if(canonical) {
			seen[sequence] = true;
			rebuilt[rebuilt_count++] = *entity;
		} else {
			/* PREPARING rolls back every tentative tag. PREPARED drops only
			 * malformed/duplicate copies and reconstructs one canonical record. */
			changed = true;
		}
	}

	if(complete_forward) {
		for(size_t sequence = 0; sequence < journal->count; sequence++) {
			if(!record_in_chunk(
				   &journal->records[sequence], chunk_x, chunk_z)
			   || seen[sequence])
				continue;
			archived_from_record(
				&rebuilt[rebuilt_count++], journal, sequence);
			seen[sequence] = true;
			changed = true;
		}
	}

	struct chunk_entities replacement = {
		.entities = rebuilt,
		.count = rebuilt_count,
	};
	bool ok = !changed || region_archive_set_chunk(
		&region, chunk_x, chunk_z, &chunk, &tile_entities, &replacement);
	/* Durability barrier (adversarial review of the F1 fix, finding 1): this
	 * replay is followed by a DURABLE level.dat with an EMPTIED inventory and
	 * then journal removal -- but set_chunk only reaches libfat's cache and
	 * region_archive_destroy is fclose without fsync. A second power cut after
	 * recovery (routine on a console that just lost power once) would lose the
	 * replayed drops AND the journal that could rebuild them, with the
	 * inventory already durably empty: total item loss. Same barrier the live
	 * checkpoint paths gained; refusing here keeps the journal and re-runs
	 * recovery on the next boot (idempotent by design). */
	/* Sync even an already-canonical retry. A previous pass may have written the
	 * canonical bytes and then lost its close evidence; `changed == false` on
	 * this pass does not prove those bytes reached durable storage. Recovery is
	 * rare and journal-gated, so the unconditional successful-archive barrier is
	 * the correct price for making the later Player/journal commit truthful. */
	if(ok)
		ok = region_archive_sync(&region);
	free(rebuilt);
	tile_entities_parse_free(&tile_entities);
	chunk_entities_free(&old_entities);
	recovery_chunk_destroy(&chunk);
	/* fclose is the last operation that can surface a deferred region write
	 * error. Once attempted, this handle is gone and no later recovery barrier
	 * can retry it, so its result must gate the Player commit and journal removal
	 * just like region_archive_sync above. Evaluate it unconditionally: a failed
	 * sync must still release the archive, and a failed close must still return
	 * false so the durable journal retains recovery authority. */
	bool closed = region_archive_destroy(&region);
	return ok && closed;
}

static bool commit_final_player(struct level_archive* level,
								const struct death_drop_journal* journal) {
	struct item_data empty_items[INVENTORY_SIZE] = {{0}};
	struct inventory empty = {
		.items = empty_items,
		.capacity = INVENTORY_SIZE,
	};
	int16_t health = journal->final_health;
	const float fall_distance = 0.0F;
	bool player_ok = level_archive_write_player(
			   level,
			   (vec3) {journal->final_position[0], journal->final_position[1],
					   journal->final_position[2]},
			   (vec2) {journal->final_rotation[0],
					   journal->final_rotation[1]},
			   NULL, journal->final_dimension)
		&& level_archive_write_inventory(level, &empty)
		&& level_archive_write_player_health(level, health)
		&& level_archive_write_player_fall_distance(level, fall_distance);
	if(player_ok && journal->final_player_spawn_known)
		player_ok = level_archive_write_player_spawn(
			level, journal->final_has_player_spawn,
			journal->final_player_spawn[0], journal->final_player_spawn[1],
			journal->final_player_spawn[2]);
	/* D6 rollback edge: the allocation's counter save can itself be the write
	 * a power cut tears, promoting an OLDER level.dat whose counter lags this
	 * journal's id. The replay just wrote region tags with that id, so bump
	 * the counter past it BEFORE the player commit -- and as its OWN saved
	 * generation (adversarial review): a single combined save would make _old
	 * the pre-bump file, so a power cut tearing the final level.dat AFTER the
	 * journal is removed would promote _old and roll the counter back below
	 * the durable region tags all over again. Two generations restore the
	 * both-survivors-carry-the-counter invariant the live alloc path gets for
	 * free; a crash between the two saves leaves the journal intact, so
	 * recovery simply reruns. (Absent counter = legacy: only bump upward.) */
	if(player_ok) {
		uint32_t counter = 0;
		if(!level_archive_read_death_tx_counter(level, &counter))
			counter = CAVEWII_DEATH_TX_LEGACY_FLOOR;
		if(counter < journal->transaction_id)
			player_ok = level_archive_write_death_tx_counter(
							level, journal->transaction_id)
				&& level_archive_save(level);
	}
	return player_ok && level_archive_save(level);
}

bool death_drop_recover_if_needed(string_t world_name,
								  struct level_archive* level) {
	if(!level)
		return false;
	/* Recovery runs on the Wii's unguarded 64 KiB server stack. Keep the ~8 KiB
	 * journal and worst-case ~2.5 KiB coordinate set in MEM2, matching live
	 * death-journal ownership, so the crash path does not consume a sixth of the
	 * stack before region/NBT helpers add their own frames. */
	struct death_drop_journal* journal = death_drop_journal_alloc();
	if(!journal)
		return false;
	bool ok = false;
	enum death_drop_journal_load_result loaded
		= death_drop_journal_load(world_name, journal);
	if(loaded == DEATH_DROP_JOURNAL_LOAD_NONE) {
		ok = true;
		goto done;
	}
	if(loaded != DEATH_DROP_JOURNAL_LOAD_OK)
		goto done;

	const size_t chunk_capacity = DEATH_DROP_JOURNAL_MAX_RECORDS * 2
		+ DEATH_DROP_JOURNAL_MAX_CONTAINER_SLOTS;
	const size_t chunk_bytes = chunk_capacity * sizeof(struct recovery_chunk);
	struct recovery_chunk* chunks = mem2_alloc(chunk_bytes);
	if(!chunks)
		goto done;
	size_t chunk_count = collect_chunks(journal, chunks);
	bool complete_forward = journal->phase == DEATH_DROP_JOURNAL_PREPARED;
	for(size_t i = 0; i < chunk_count; i++) {
		if(!recover_chunk(world_name, journal, chunks[i].x, chunks[i].z,
						  complete_forward))
			goto chunks_done;
#ifdef CAVEWII_DEATH_DROP_TEST
		/* Model a process dying after this chunk's region write returned but
		 * before the next owner was touched. The journal deliberately remains. */
		if(s_fail_after_chunks > 0 && --s_fail_after_chunks == 0)
			goto chunks_done;
#endif
	}

	if(complete_forward
	   && !clear_legacy_container_sources(world_name, journal))
		goto chunks_done;
	if(complete_forward && !commit_final_player(level, journal))
		goto chunks_done;
	ok = death_drop_journal_remove(world_name);

chunks_done:
	mem2_free_sized(chunks, chunk_bytes);
done:
	death_drop_journal_free(journal);
	return ok;
}
