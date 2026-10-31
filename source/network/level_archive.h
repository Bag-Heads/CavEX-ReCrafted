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

#ifndef LEVEL_ARCHIVE_H
#define LEVEL_ARCHIVE_H

#include <m-lib/m-string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../cNBT/nbt.h"
#include "../cglm/cglm.h"

#include "../item/inventory.h"
#include "../item/item_types.h"
#include "../world.h"

struct level_archive_tag {
	const char* name;
	nbt_type type;
};

#define LEVEL_NAME                                                             \
	(struct level_archive_tag) {                                               \
		".Data.LevelName", TAG_STRING                                          \
	}
#define LEVEL_TIME                                                             \
	(struct level_archive_tag) {                                               \
		".Data.Time", TAG_LONG                                                 \
	}
// Weather (WorldInfo: raining/rainTime/thundering/thunderTime, Beta's own tag
// names, [740c583: WorldInfo.updateTagCompound]). Strengths are NOT persisted
// (runtime only, per WEATHER_DISPATCH.md) -- only the two flags + two timers
// round-trip. The bundled level.dat template (tools/gen_level_template.py)
// already carries all four tags zeroed, so the plain strict read/write below
// (which requires the node to pre-exist) works for every CaveWii-created
// world without a migration writer; a real imported Beta save carries them
// natively too.
#define LEVEL_WEATHER_RAINING                                                 \
	(struct level_archive_tag) {                                               \
		".Data.raining", TAG_BYTE                                              \
	}
#define LEVEL_WEATHER_RAIN_TIME                                               \
	(struct level_archive_tag) {                                               \
		".Data.rainTime", TAG_INT                                              \
	}
#define LEVEL_WEATHER_THUNDERING                                              \
	(struct level_archive_tag) {                                               \
		".Data.thundering", TAG_BYTE                                           \
	}
#define LEVEL_WEATHER_THUNDER_TIME                                            \
	(struct level_archive_tag) {                                               \
		".Data.thunderTime", TAG_INT                                           \
	}
#define LEVEL_DISK_SIZE                                                        \
	(struct level_archive_tag) {                                               \
		".Data.SizeOnDisk", TAG_LONG                                           \
	}
#define LEVEL_LAST_PLAYED                                                      \
	(struct level_archive_tag) {                                               \
		".Data.LastPlayed", TAG_LONG                                           \
	}
#define LEVEL_RANDOM_SEED                                                      \
	(struct level_archive_tag) {                                               \
		".Data.RandomSeed", TAG_LONG                                           \
	}
// CaveWii generator pin (P-1, 2026-06-11): which terrain generator owns this
// world, forever -- per-world pinning is what makes a future second (Release
// 1.0) generator possible without Mojang-style frontier seams. ABSENT tag
// reads as B173 by definition (tag-less worlds predate any second generator:
// imported real Beta saves + pre-tag CaveWii saves). New worlds get it from
// the level.dat template (tools/gen_level_template.py) AND the explicit
// create-time write -- which doubles as a loud staleness check, since
// level_archive_write cannot create missing nodes.
#define LEVEL_CAVEWII_GENERATOR                                                \
	(struct level_archive_tag) {                                               \
		".Data.CaveWiiGenerator", TAG_INT                                      \
	}
#define CAVEWII_GENERATOR_B173 1 // byte-faithful Beta 1.7.3 (the only one today)
// CaveWii-owned persistence schema. Imported Beta and pre-version CaveWii
// worlds have no tag and are treated as legacy version 0. Version 2 changed
// wall-sign metadata from the port-private 4..7 encoding to Beta's raw 2..5;
// server_local completes or safely declines the eager region migration before
// explicitly stamping it. Ordinary saves preserve the existing version.
#define LEVEL_CAVEWII_SAVE_VERSION                                             \
	(struct level_archive_tag) {                                               \
		".Data.CaveWiiSaveVersion", TAG_INT                                    \
	}
#define CAVEWII_SAVE_VERSION_LEGACY 0
// Version 2: wall-sign metadata moved to Beta's raw 2..5 encoding.
#define CAVEWII_SAVE_VERSION_WALL_SIGNS 2
// Version 3 (FH-07): the fork's iron chest moved off Beta's id 95
// (BlockLockedChest) to the net-new 200. server_local runs this region
// rewrite only from an exactly-version-2 world, so a refused wall-sign
// migration keeps BOTH stages retryable on later loads.
#define CAVEWII_SAVE_VERSION_IRON_CHEST_IDS 3
// Version 4 (FH-17): dungeon mob spawners move off a metadata-encoded mob
// type (2 bits in the spawner block's metadata nibble) to a faithful
// MobSpawner tile entity (Beta TileEntityMobSpawner's EntityId/Delay tags).
// server_local runs this region rewrite only from an exactly-version-3 world.
#define CAVEWII_SAVE_VERSION_MOB_SPAWNER_TES 4
// Version 5 (PAR-BED-META): bed metadata moved from the port-private
// index<<1 over {+Z,-Z,+X,-X} to Beta's own layout (dir meta&3, 0x4
// occupied, 0x8 head; block/bed_meta.h). server_local converts only a
// CaveWii-created world, only from exactly version 4; imported Beta worlds
// are stamped without a byte of region data changing.
#define CAVEWII_SAVE_VERSION_BED_META 5
#define CAVEWII_SAVE_VERSION_CURRENT CAVEWII_SAVE_VERSION_BED_META
/* Crash-retry journal for the wall-sign sweep. This is NOT provenance: it is
 * created only after a complete read-only scan has already established legacy
 * encoding, and lets that established decision survive a mid-sweep crash. */
#define LEVEL_CAVEWII_WALL_SIGN_LEGACY_INTENT                                  \
	(struct level_archive_tag) {                                               \
		".Data.CaveWiiWallSignLegacyIntent", TAG_BYTE                          \
	}
// Monotonic death-drop transaction-id counter (bug-hunt D6): the highest id
// ever ISSUED, persisted BEFORE any tag carrying that id can reach a region
// file. Region CaveWiiDeathTransaction tags persist forever, but the old
// allocator scanned only LIVE entities -- a chunk whose archived item failed
// to materialize (spawn OOM) could get its stale same-id tag silently deleted
// by a later recovery's duplicate filter. Absent tag = legacy/imported world:
// seed the floor below, which clears every plausible pre-fix on-disk tag
// (pre-fix ids restarted at 1 each session). Stored TAG_LONG carrying a u32,
// the entity-archive u32 convention; id 0 stays reserved.
#define LEVEL_CAVEWII_DEATH_TX_COUNTER                                         \
	(struct level_archive_tag) {                                               \
		".Data.CaveWiiDeathTxCounter", TAG_LONG                                \
	}
#define CAVEWII_DEATH_TX_LEGACY_FLOOR 65536u
// PER-WORLD GAME MODE (net-new; docs/ui_design/create_world_console_2026-09-01.md
// section 3.1, maintainer decision 2026-09-01). Beta 1.7.3 has NO game mode of
// any kind -- there is no GameType tag to be faithful to (Beta 1.8 introduced
// it), so this lives in the CaveWii-owned tag namespace next to
// CaveWiiGenerator / CaveWiiSaveVersion rather than squatting a name a real
// imported Beta save could carry. ABSENT TAG = SURVIVAL, by decision: every
// world that predates this tag (imported Beta saves and every CaveWii world
// created before 2026-09-01) is a survival world, so no migration pass is
// needed and nothing is written until a creative world is created.
#define LEVEL_GAME_MODE                                                        \
	(struct level_archive_tag) {                                               \
		".Data.CaveWiiGameMode", TAG_BYTE                                      \
	}
#define LEVEL_GAME_MODE_SURVIVAL 0
#define LEVEL_GAME_MODE_CREATIVE 1
#define LEVEL_PLAYER_HEALTH                                                    \
	(struct level_archive_tag) {                                               \
		".Data.Player.Health", TAG_SHORT                                       \
	}
#define LEVEL_PLAYER_FALL_DISTANCE                                             \
	(struct level_archive_tag) {                                               \
		".Data.Player.FallDistance", TAG_FLOAT                                  \
	}
#define LEVEL_WORLD_SPAWNX                                                     \
	(struct level_archive_tag) {                                               \
		".Data.SpawnX", TAG_INT                                                \
	}
#define LEVEL_WORLD_SPAWNY                                                     \
	(struct level_archive_tag) {                                               \
		".Data.SpawnY", TAG_INT                                                \
	}
#define LEVEL_WORLD_SPAWNZ                                                     \
	(struct level_archive_tag) {                                               \
		".Data.SpawnZ", TAG_INT                                                \
	}
#define LEVEL_PLAYER_SPAWNX                                                    \
	(struct level_archive_tag) {                                               \
		".Data.Player.SpawnX", TAG_INT                                         \
	}
#define LEVEL_PLAYER_SPAWNY                                                    \
	(struct level_archive_tag) {                                               \
		".Data.Player.SpawnY", TAG_INT                                         \
	}
#define LEVEL_PLAYER_SPAWNZ                                                    \
	(struct level_archive_tag) {                                               \
		".Data.Player.SpawnZ", TAG_INT                                         \
	}
#define LEVEL_PLAYER_POSITION                                                  \
	(struct level_archive_tag) {                                               \
		".Data.Player.Pos", TAG_LIST                                           \
	}
#define LEVEL_PLAYER_ROTATION                                                  \
	(struct level_archive_tag) {                                               \
		".Data.Player.Rotation", TAG_LIST                                      \
	}
#define LEVEL_PLAYER_VELOCITY                                                  \
	(struct level_archive_tag) {                                               \
		".Data.Player.Motion", TAG_LIST                                        \
	}
#define LEVEL_PLAYER_DIMENSION                                                 \
	(struct level_archive_tag) {                                               \
		".Data.Player.Dimension", TAG_INT                                      \
	}

#define LEVEL_PLAYER_INVENTORY                                                 \
	(struct level_archive_tag) {                                               \
		".Data.Player.Inventory", TAG_LIST                                     \
	}
#define LEVEL_PLAYER_ITEM_ID                                                   \
	(struct level_archive_tag) {                                               \
		".id", TAG_SHORT                                                       \
	}
#define LEVEL_PLAYER_ITEM_COUNT                                                \
	(struct level_archive_tag) {                                               \
		".Count", TAG_BYTE                                                     \
	}
#define LEVEL_PLAYER_ITEM_DURABILITY                                           \
	(struct level_archive_tag) {                                               \
		".Damage", TAG_SHORT                                                   \
	}
#define LEVEL_PLAYER_ITEM_SLOT                                                 \
	(struct level_archive_tag) {                                               \
		".Slot", TAG_BYTE                                                      \
	}

struct level_archive {
	nbt_node* data;
	string_t file_name;
	bool modified;
	bool lossless_blocked; /* imported scope failed byte-exact write preflight */
	bool inventory_write_refused; /* prevents destructor/checkpoint partial saves */
	/* G9 R19: a background (autosave) save queued on the deferred IO worker,
	 * or NULL. Server-thread owned; every synchronous save/destroy settles it
	 * first. Always NULL while experimental.g9_save_io_int is off. */
	void* pending_job;
	/* dropin-1029: level_archive_save_batch_begin/end. While depth > 0,
	 * level_archive_upgrade_save_version stamps in memory only and sets
	 * save_batch_pending; batch_end writes ONE save. */
	uint8_t save_batch_depth;
	bool save_batch_pending;
};

/* Shared trust-boundary validation for both persisted player state and live
 * PLAYER_POS RPCs. Values outside these bounds are rejected before any float-
 * to-integer conversion or authoritative mutation. */
bool level_player_position_values_valid(double x, double y, double z);
bool level_player_rotation_values_valid(float x, float y);
bool level_player_velocity_values_valid(float x, float y, float z);

bool level_archive_create(struct level_archive* la, string_t filename);
/* G9 R19: the AUTOSAVE's level.dat save. With deferred save IO on, the write,
 * fsync, verify and _old/_new rotation are queued on the IO worker (the clone
 * of the tree is taken here, frozen); the call returns once queued, the
 * modified bit is re-set if the job later fails. Off: exactly
 * level_archive_save. Never use it where a durable success is required. */
bool level_archive_save_background(struct level_archive* la);
/* Barrier: wait for (and reconcile) a queued background save. */
void level_archive_settle_pending(struct level_archive* la);
// Synthesize a brand-new world's level.dat from the bundled Beta template:
// `directory` is the world folder (level.dat is created inside it), `seed` the
// world seed, `spawn` the player spawn position (SpawnXYZ is floor(spawn)). The
// file is written by the subsequent level_archive_destroy. Returns success.
bool level_archive_create_new(struct level_archive* la, string_t directory,
							  int64_t seed, vec3 spawn);
bool level_archive_read(struct level_archive* la, struct level_archive_tag tag,
						void* result, size_t length);
// Current Beta-file footprint (not Beta's incremental WorldInfo.sizeOnDisk):
// level.dat[+_old/_new] + region/*.mcr in both dimensions, summed fresh.
// CaveWii journal/index sidecars and unrelated files are excluded.
// A documented deviation from Beta's own incremental delta
// counter -- see the definition in level_archive.c for the full rationale.
// Best-effort: an unreadable/malformed la->file_name or missing directories
// fail closed to a partial (never negative, never a crash) total.
int64_t level_archive_disk_size(struct level_archive* la);
// Returns LEGACY (0) when the tag is absent. level_archive_create rejects a
// malformed, negative, or newer-than-supported version before this is called.
int32_t level_archive_get_save_version(const struct level_archive* la);
/* True only when the generator tag installed by level_archive_create_new is
 * present and valid. Ordinary saves never create that tag, so an imported
 * real-Beta world does not acquire this provenance merely by being saved. */
bool level_archive_was_created_by_cavewii(const struct level_archive* la);
/* dropin-1029: where a world came from, for the import convert notice.
 *   CAVEWII        : CaveWiiGenerator tag (every CaveWii world since c60cc0dac,
 *                    2026-06-11).
 *   CAVEWII_LEGACY : no generator tag but POSITIVE CaveWii-made evidence, which
 *                    wins over any Beta evidence (review M1): a Player
 *                    compound born in a CaveWii template (no Score/Sleeping/
 *                    SleepTimer -- Beta 1.7.3 EntityPlayer.writeEntityToNBT
 *                    :330-331 writes Sleeping+SleepTimer unconditionally), or a
 *                    probed chunk with a .Level.CaveWii* tag.
 *   BETA           : everything else (no CaveWii evidence). LastUpdate is not
 *                    used: a real JAR-made world keeps 0 on chunks saved at
 *                    world time 0 and never modified.
 * The chunk probe is BOUNDED (LEVEL_ORIGIN_PROBE_CHUNKS chunks from at most
 * two overworld region files, read-only) and the answer is cached per
 * level.dat path + size + mtime (review M2: the world list calls this per row
 * on every rebuild). A CaveWii-resaved chunk outside the probe is missed; the
 * chain itself never depends on this answer. */
enum level_world_origin {
	LEVEL_ORIGIN_CAVEWII,
	LEVEL_ORIGIN_BETA,
	LEVEL_ORIGIN_CAVEWII_LEGACY,
};
#define LEVEL_ORIGIN_PROBE_CHUNKS 16
bool level_archive_player_beta_born(const struct level_archive* la);
enum level_world_origin level_archive_world_origin(const struct level_archive* la);
/* dropin-1029: nestable level.dat write batch (see the struct fields). End
 * writes the single save if any stamp was deferred; false = that save failed
 * (the stamps stay dirty in memory, exactly like a failed immediate stamp). */
void level_archive_save_batch_begin(struct level_archive* la);
bool level_archive_save_batch_end(struct level_archive* la);
/* dropin-1029 review: write any deferred stamp NOW, inside the batch. The
 * save-version chain calls it right after a stage that rewrote region bytes,
 * so that stage's version is durable before the next stage starts (the crash
 * barrier the per-stage saves gave). No-op stamps stay batched. */
bool level_archive_save_batch_flush(struct level_archive* la);
#ifdef CAVEWII_SERVER_LOCAL_TEST
/* Simulated power cut: forget the batch without saving. */
void level_archive_save_batch_abandon(struct level_archive* la);
#endif
bool level_archive_has_wall_sign_legacy_intent(
	const struct level_archive* la);
/* Durably record a previously established legacy-encoding decision before
 * changing any region. Ordinary saves never create this journal tag. */
bool level_archive_record_wall_sign_legacy_intent(struct level_archive* la);
// Persist `version` (a CAVEWII_SAVE_VERSION_* value) only after every external
// archive participating in that schema migration is durable. Never lowers an
// existing version. This marks the tree modified and uses the normal
// level.dat_new/_old rotation; callers must fail the world load if it fails.
bool level_archive_upgrade_save_version(struct level_archive* la,
										int32_t version);
bool level_archive_write(struct level_archive* la, struct level_archive_tag tag,
						 void* data);
/* FallDistance was added after CaveWii saves already existed. Unlike the
 * generic strict writer, this narrowly scoped writer creates an absent FLOAT
 * under Data.Player so legacy worlds migrate on their next checkpoint. A
 * present wrong-type tag is rejected as corruption. */
bool level_archive_write_player_fall_distance(struct level_archive* la,
										 float fall_distance);
/* Per-world game mode (LEVEL_GAME_MODE). The reader is TOTAL: an absent tag,
 * a wrong-type node, or an out-of-range payload all read as SURVIVAL (false)
 * -- the documented "missing tag = survival" rule, which also fails closed
 * (a corrupt byte can never silently hand out creative). The writer creates
 * an absent TAG_BYTE under Data (allocate-before-link, OOM-safe) exactly like
 * the FallDistance migration writer, so a survival world that is later
 * created as creative -- and every legacy world -- migrates on write without
 * a separate pass. */
bool level_archive_is_creative(const struct level_archive* la);
bool level_archive_write_game_mode(struct level_archive* la, bool creative);
/* Death-transaction counter (LEVEL_CAVEWII_DEATH_TX_COUNTER). `read` returns
 * false when the tag is absent (legacy world -- caller seeds
 * CAVEWII_DEATH_TX_LEGACY_FLOOR) or malformed/out-of-range; `write` creates
 * the absent TAG_LONG under Data (allocate-before-link, OOM-safe) exactly
 * like the FallDistance migration writer above. */
bool level_archive_read_death_tx_counter(struct level_archive* la,
										 uint32_t* out);
bool level_archive_write_death_tx_counter(struct level_archive* la,
										  uint32_t value);
/* Beta EntityPlayer owns a separate, optional bed-spawn triple under Player.
 * `read` returns true for both a valid triple and normal all-absent state;
 * malformed/partial triples return false and commit present=false. `write`
 * transactionally creates/replaces all three tags, or removes them when
 * present=false. */
bool level_archive_read_player_spawn(struct level_archive* la, bool* present,
									 int32_t* x, int32_t* y, int32_t* z);
bool level_archive_write_player_spawn(struct level_archive* la, bool present,
									  int32_t x, int32_t y, int32_t z);
/* Player health on load, Beta-exact (IMP-PLAYER-DEFAULT). No Data.Player
 * compound: Beta never calls readFromNBT, the fresh EntityPlayer keeps 20 HP
 * (= MAX_PLAYER_HEALTH here). Player present but Health absent/wrong type:
 * EntityLiving.readEntityFromNBT sets 10 HP (= MAX_PLAYER_HEALTH / 2). A
 * present Health is Beta HALF-HEARTS (EntityLiving.java:558/565) and is
 * converted to CaveWii units (x8, clamped), unless the CaveWii-only
 * Data.Player.CaveWiiHealth still converts to it, in which case that exact
 * value is returned (0 = quit-while-dead). Always true. */
bool level_archive_read_player_health(struct level_archive* la, int16_t* out);
/* PLAYER-HEALTH-NBT: CaveWii units (MAX_PLAYER_HEALTH per 20 half-hearts)
 * <-> Beta half-hearts. to_beta: exact for multiples of 8; otherwise ceil for
 * > 0 (a living player never saves dead), truncation toward 0 for <= 0.
 * from_beta: x8 in int32, clamped to [INT16_MIN, MAX_PLAYER_HEALTH]. */
int16_t level_archive_health_to_beta(int16_t units);
int16_t level_archive_health_from_beta(int16_t half);
/* The ONLY sanctioned player-health writer: Health = Beta half-hearts
 * (TAG_SHORT) plus CaveWiiHealth = exact units, created if absent, OOM-safe.
 * Do not write LEVEL_PLAYER_HEALTH raw with CaveWii units. */
bool level_archive_write_player_health(struct level_archive* la,
									   int16_t units);
/* Health present as TAG_SHORT -> true with CaveWii units: the consistent
 * CaveWiiHealth marker if any; else, when `legacy_units_without_marker` and
 * no marker exists, Health verbatim (pre-fix CaveWii-only records such as
 * the split-screen guest); else Health converted from half-hearts. Missing
 * Player/Health -> false. */
bool level_archive_read_player_health_strict(struct level_archive* la,
											 bool legacy_units_without_marker,
											 int16_t* out);
/* World-load migration hook (run first in the save-version chain): a
 * marker-less (pre-fix) Player.Health is classified ONCE and rewritten in the
 * tree as half-hearts + marker. Classification (level_archive.c,
 * level_player_legacy_health_units): Player born in Beta (has Beta's
 * unconditional Sleeping/SleepTimer/Score) -> half-hearts unless > 20 (Beta
 * caps at 20, so only CaveWii units exceed it); born in a CaveWii template
 * (none of them) -> units, except a raw 20 -> full. Marks the tree modified;
 * the next stamp or checkpoint persists it. False only on OOM / a malformed
 * tree. */
bool level_archive_normalize_player_health(struct level_archive* la,
										   bool* converted);
bool level_archive_write_player(struct level_archive* la, vec3 position,
								vec2 rotation, vec3 velocity,
								enum world_dim dimension);
/* M1 (A2-T COMMIT): an exact copy of the Player record fields a COMMIT
 * rewrites (Pos, Rotation, Dimension) plus the archive modified flag. A
 * record written and then refused by level_archive_save is restored verbatim
 * so no later bare save can persist it. Both return false on a malformed
 * tree (restore then changes nothing). */
struct level_archive_player_record {
	double pos[3];
	float rot[2];
	int32_t dimension;
	bool modified;
	bool valid;
};
bool level_archive_player_record_snapshot(
	struct level_archive* la, struct level_archive_player_record* rec);
bool level_archive_player_record_restore(
	struct level_archive* la, const struct level_archive_player_record* rec);
bool level_archive_write_inventory(struct level_archive* la,
								   struct inventory* inventory);
/* CR-02, docs/audit_2026-08-20_code_review.md: TRANSACTIONAL. Validates the
 * whole archived inventory list before touching `inventory`; on `false` the
 * caller's live inventory is untouched (not cleared, not partially loaded). */
bool level_archive_read_inventory(struct level_archive* la,
								  struct inventory* inventory);
bool level_archive_read_player(struct level_archive* la, vec3 position,
							   vec2 rotation, vec3 velocity,
							   enum world_dim* dimension);
// Persist the (modified) tree to disk NOW via the safe level.dat_new/_old
// rotation, clearing `modified` on success -- the autosave entry point.
// destroy still flushes too; this lets a long session checkpoint without
// tearing the archive down. Returns false if the write failed (old file
// left intact).
bool level_archive_save(struct level_archive* la);

void level_archive_destroy(struct level_archive* la);
// Free an open archive without flushing its modified in-memory tree. Portal
// rollback uses this to abandon a destination commit that never completed.
void level_archive_discard(struct level_archive* la);

#endif
