/*
	Copyright (c) 2026 CaveWii_v2 contributors

	Crash-safe ownership journal for Nether death drops. Beta determines which
	stacks drop; this file only makes the cross-file level/region commit atomic.
*/

#ifndef DEATH_DROP_JOURNAL_H
#define DEATH_DROP_JOURNAL_H

#include <m-lib/m-string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../item/item_types.h"
#include "../world.h"

#define DEATH_DROP_JOURNAL_MAX_RECORDS 128
#define DEATH_DROP_JOURNAL_MAX_CONTAINER_SLOTS 54

enum death_drop_journal_phase {
	DEATH_DROP_JOURNAL_PREPARING = 1,
	DEATH_DROP_JOURNAL_PREPARED = 2,
};

/* Exact pre-transfer owner. Recovery normally completes PREPARED forward, but
 * retaining provenance makes every journal-owned stack auditable and prevents
 * a future rollback path from guessing whether a cursor/storage stack belonged
 * to the player or a block container. */
enum death_drop_owner_kind {
	DEATH_DROP_OWNER_PLAYER_SLOT = 1,
	DEATH_DROP_OWNER_PLAYER_CURSOR,
	DEATH_DROP_OWNER_ACTIVE_CRAFT_SLOT,
	DEATH_DROP_OWNER_ACTIVE_CURSOR,
	DEATH_DROP_OWNER_CONTAINER_STORAGE,
};

struct death_drop_provenance {
	uint8_t kind;
	uint8_t window;
	int16_t slot;
	int16_t owner_slot; /* meaningful only for container-storage owners */
	int32_t owner_x;
	int32_t owner_y;
	int32_t owner_z;
};

/* Exact post-close tile-entity state for a valid open block container. The
 * death decision is durable before the GUI is closed, so recovery must be able
 * to replay storage edits that existed only in the live window at that point. */
struct death_drop_container_slot {
	struct item_data item;
	uint8_t window;
	int16_t owner_slot;
	int32_t owner_x;
	int32_t owner_y;
	int32_t owner_z;
};

struct death_drop_record {
	struct item_data item;
	float pos[3];
	float vel[3];
	struct death_drop_provenance provenance;
};

struct death_drop_journal {
	enum death_drop_journal_phase phase;
	uint32_t transaction_id; /* zero is reserved for ordinary EntityItem records */
	enum world_dim source_dimension;
	float final_position[3];
	float final_rotation[2];
	enum world_dim final_dimension;
	int16_t final_health;
	/* Version-3 final EntityPlayer spawn state. `known=false` preserves the
	 * existing Player.Spawn tags when recovering a version-2 journal. */
	bool final_player_spawn_known;
	bool final_has_player_spawn;
	int32_t final_player_spawn[3];
	size_t count;
	struct death_drop_record records[DEATH_DROP_JOURNAL_MAX_RECORDS];
	size_t container_count;
	struct death_drop_container_slot
		container_slots[DEATH_DROP_JOURNAL_MAX_CONTAINER_SLOTS];
};

/* Large runtime journals live in MEM2, never in server_local's main-thread
 * stack frame. The returned object is zeroed; free is NULL-safe. */
struct death_drop_journal* death_drop_journal_alloc(void);
void death_drop_journal_free(struct death_drop_journal* journal);

enum death_drop_journal_load_result {
	DEATH_DROP_JOURNAL_LOAD_NONE = 0,
	DEATH_DROP_JOURNAL_LOAD_OK,
	DEATH_DROP_JOURNAL_LOAD_ERROR,
};

enum death_drop_journal_store_result {
	DEATH_DROP_JOURNAL_STORE_UNPUBLISHED_FAILURE = 0,
	DEATH_DROP_JOURNAL_STORE_VALID_PUBLISHED_UNSYNCED,
	DEATH_DROP_JOURNAL_STORE_DURABLE,
};

/* Writes death_drops.dat_new, flushes it to stable storage, then replaces the
 * committed journal through the same current/_old rotation as level.dat. A
 * published-but-unsynced result means current names a valid new journal, but
 * reopening/fsyncing/closing that committed name did not prove durability. */
enum death_drop_journal_store_result death_drop_journal_store(
	string_t world_name, const struct death_drop_journal* journal);
enum death_drop_journal_load_result death_drop_journal_load(
	string_t world_name, struct death_drop_journal* journal);
bool death_drop_journal_remove(string_t world_name);

#ifdef CAVEWII_DEATH_DROP_TEST
enum death_drop_journal_test_fail_stage {
	DEATH_DROP_JOURNAL_TEST_FAIL_NONE = 0,
	DEATH_DROP_JOURNAL_TEST_FAIL_WRITE,
	DEATH_DROP_JOURNAL_TEST_FAIL_RENAME,
	DEATH_DROP_JOURNAL_TEST_FAIL_COMMITTED_SYNC,
	DEATH_DROP_JOURNAL_TEST_FAIL_REMOVE,
	DEATH_DROP_JOURNAL_TEST_FAIL_REMOVE_PATH,
	DEATH_DROP_JOURNAL_TEST_FAIL_COUNT,
};
void death_drop_journal_test_fail_next(
	enum death_drop_journal_test_fail_stage stage);
void death_drop_journal_test_fail_after(
	enum death_drop_journal_test_fail_stage stage, unsigned occurrence);
#endif

#endif
