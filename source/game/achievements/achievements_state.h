/* Copyright (c) 2026 CaveWii_v2 contributors */
/*
 * Per-profile achievement / statistic STATE -- the data only.
 *
 * Spec: docs/achievements_design.md section 2 (the CaveWii data model). This
 * translation unit deliberately holds NO event handling, no ring, no
 * coalescer, no notices and no screen API: those are briefs B2 (events,
 * lifecycle) and B4 (notice + screen data API). What lives here is the fixed
 * per-profile storage, the saturating counter arithmetic, the canonical-block
 * fold and the two achievement predicates, so B1's persistence layer and B2's
 * rules layer share one definition instead of two.
 *
 * No heap: every array is fixed-size and sized from the table in
 * stats_table.h (23 general ids, 256 block ids, ITEMS_MAX item ids x3, 16
 * achievements). One struct is ~57 KB, so callers keep profiles in static or
 * allocated storage, never on a Wii thread stack [design 2 "no heap in the hot
 * path"].
 */

#ifndef CW_ACHIEVEMENTS_STATE_H
#define CW_ACHIEVEMENTS_STATE_H

#include <stdbool.h>
#include <stdint.h>

/* ITEMS_MAX (2304, source/item/items.h) is included rather than re-typed: a
 * second copy of that bound is exactly how a save format and a registry drift
 * apart. It costs this header item_types.h's transitive includes. */
#include "../../item/items.h"
#include "stats_table.h"

/* profile_id: 1..16 chars of [a-z0-9_] plus the terminator [design 3]. */
#define CW_PROFILE_ID_MAX 16

/* ---- Foreign records [design 4, R1 HIGH 2] -------------------------------
 * A crc-VALID file may carry records this build has no slot for: an id past
 * one of the arrays, or a record letter a later version added. Dropping them
 * on the next save is permanent, invisible progress loss, so they are kept
 * VERBATIM here and re-emitted by the writer.
 *
 * The line bound is derived, not guessed: the widest record the format can
 * produce is `a <id> <local> <remote> <L|R>` with three ten-digit fields =
 * 1 + 1 + 10 + 1 + 10 + 1 + 10 + 1 + 1 = 36 characters, so 64 bytes leaves 27
 * of headroom for the extra spaces a hand-written or foreign writer may use.
 * A line that does NOT fit, or a 65th foreign line, is an overflow: the
 * profile is write-refused (fail closed) rather than re-emitted short. */
#define CW_STATS_FOREIGN_MAX 64
#define CW_STATS_FOREIGN_LINE_MAX 64

/* stats_profile::write_refused -- WHY the writer must not touch this name.
 * 0 is "write normally"; every other value is a refusal the save layer turns
 * into its own status so the UI can say which one it is [design 4 "Load"]. */
enum stats_write_refusal {
	STATS_WRITE_ALLOWED = 0,
	/* The on-disk file declares a version this build does not understand. */
	STATS_WRITE_REFUSED_NEWER = 1,
	/* Records this build cannot hold overflowed the `foreign` list, so a save
	 * would silently drop them (R1 HIGH 2). */
	STATS_WRITE_REFUSED_FOREIGN = 2,
};

/* Every counter is a pair: which column an observation lands in is decided at
 * bind time by the session mode, never by the producer [design 6].
 * LOCAL_WORLD -> local, REMOTE_SERVER -> remote. */
struct stats_pair {
	uint32_t local;
	uint32_t remote;
};

enum stats_column {
	STATS_COLUMN_LOCAL = 0,
	STATS_COLUMN_REMOTE = 1,
};

/* Which side first unlocked an achievement; written as the trailing L|R of an
 * `a` record [design 4]. STATS_FIRST_NONE means "never unlocked". */
enum stats_first {
	STATS_FIRST_NONE = 0,
	STATS_FIRST_LOCAL = 'L',
	STATS_FIRST_REMOTE = 'R',
};

struct stats_profile {
	/* General stats, indexed by the CW_STAT_ROWS row order (NOT by raw id);
	 * stat_general_index() maps an id to a slot. */
	struct stats_pair general[CW_STAT_COUNT];
	struct stats_pair mine[CW_STAT_BLOCK_COUNT];
	struct stats_pair craft[ITEMS_MAX];
	struct stats_pair use[ITEMS_MAX];
	struct stats_pair brk[ITEMS_MAX];
	struct stats_pair ach[CW_ACH_COUNT];
	/* Parallel to ach[]: enum stats_first. Needed because design 4's `a`
	 * record carries the first-unlock provenance letter, so it has to
	 * round-trip through save/load; design 2's field list predates that
	 * record and does not mention it. */
	uint8_t ach_first[CW_ACH_COUNT];
	char profile_id[CW_PROFILE_ID_MAX + 1];
	/* "Something this profile should hold is not in it": B2 sets it when an
	 * event had to be dropped, the loader sets it when the file carried a
	 * record this build cannot represent. PERSISTED as the `incomplete 1`
	 * header line, so the warning survives the round trip instead of being
	 * cleared by the first save (R1 HIGH 2). */
	uint8_t progress_incomplete;
	/* enum stats_write_refusal. stats_profile_save() honours it and refuses to
	 * write, so newer progress is never overwritten with an empty file and a
	 * foreign record is never silently dropped [design 4 "Load"]. */
	uint8_t write_refused;
	/* The MAIN file this profile was loaded from failed our own parser. The
	 * next save must NOT rotate that file into `.bak` -- `.bak` only ever
	 * receives a file our writer produced -- it removes it instead (R1 HIGH
	 * 1). The owner clears the flag after its first successful save (B2's
	 * flush_slot does); the writer takes the profile by const pointer and
	 * cannot. */
	uint8_t main_was_corrupt;
	/* Records the loader could not represent, kept verbatim (without their
	 * line ending) and re-emitted in load order after every known counter. */
	uint8_t foreign_count;
	char foreign[CW_STATS_FOREIGN_MAX][CW_STATS_FOREIGN_LINE_MAX];
};

/* Zero every counter and bind the profile key. `profile_id` may be NULL. A key
 * that is not stats_profile_id_valid() -- NULL, empty, longer than
 * CW_PROFILE_ID_MAX, or out of charset -- leaves the key EMPTY rather than
 * truncating it (R1 MED 3): a truncated key is one the writer would accept and
 * the reader could never open again, so the storage layer refuses to invent
 * one. The profile is then unwritable (STATS_SAVE_ERR_BAD_ARG) until a valid
 * key is set, which is the honest outcome of an invalid key. */
void stats_profile_reset(struct stats_profile* p, const char* profile_id);

/* The ONE key rule [design 3]: 1..CW_PROFILE_ID_MAX bytes of [a-z0-9_]. Both
 * the bind path (B2) and both file paths (B1 load/save) go through this, so a
 * key can never be accepted by one and truncated by the other. */
bool stats_profile_id_valid(const char* profile_id);

/* Beta's Java int wraps; CaveWii saturates [design 2, documented deviation,
 * no gameplay effect -- UINT32_MAX centimetres is 42 949 km]. Amount 0 is a
 * no-op. */
void stats_counter_add(struct stats_pair* p, enum stats_column column,
					   uint32_t amount);

/* local + remote computed in uint64 and saturated back to uint32 [design 2]. */
uint32_t stats_counter_total(const struct stats_pair* p);

/* Fold a block id onto the counter Beta would actually increment
 * [design 2]: StatList.func_25153_a's registration gate (`blocksList[j] !=
 * null && getEnableStats()`), then the alias fold. Returns
 * CW_STAT_BLOCK_NONE when the block has no counter at all -- its own
 * registration is suppressed (a disableStats() block), the cell it folds onto
 * is unregistered, Beta has no block at that id (0, and 97..255, which is
 * where CaveWii's net-new blocks live), or the id is outside 0..255. THE MINE
 * ARRAY ONLY -- see stat_alias_block_in(). */
uint16_t stat_canonical_block(uint16_t id);

/* Beta's registration + alias fold for a block id, in the NAMED array: the
 * counter id a count for `id` lands on there, or CW_STAT_BLOCK_NONE when Beta
 * registers no counter for it in that array at all.
 *
 * StatList.<clinit> calls replaceAllSimilarBlocks() on all four arrays, so a
 * block crafted/used/broken under an aliased id counts on the target's
 * counter exactly as a mined one does. But each array is built by its OWN
 * registration loop, and only the mine one consults `getEnableStats()`:
 * func_25155_a (use) registers every non-null Item.itemsList entry, so a
 * wooden door (64), sign (63), cake (92), repeater (93) and bed (26) -- all
 * in the no-mine-stat set -- still carry a useItem counter. Folding the three
 * item arrays through stat_canonical_block() would therefore DROP counters
 * Beta keeps, so they fold through this function instead.
 *
 * WHY THE ARRAY IS A PARAMETER (B3): replaceSimilarBlocks() branches on which
 * SIDE of the pair is registered in the array it is folding, so the direction
 * is not a property of the pair alone. craftItem registers only
 * recipe/smelting outputs, and for the two pairs whose source is an output and
 * whose target is not -- jack-o-lantern 91 / pumpkin 86 and active redstone
 * torch 76 / idle 75 -- Beta takes the `a[i] != null && a[j] == null` branch
 * and the shared counter is the SOURCE's id (CW_BLOCK_CRAFT_INVERT_ROWS in
 * stats_table.h carries those two rows with their CraftingManager citations,
 * now as the pinned expectation rather than the driver). Every other pair, and
 * every pair in the use/break arrays, folds in the table's direction.
 *
 * THE REGISTRATION GATES (F3, closing the deviation B1/B3 recorded here): a
 * counter exists in an array exactly where Beta's loop for THAT array made a
 * StatBase, so this returns CW_STAT_BLOCK_NONE -- not a folded id -- where
 * Beta's addStat would find a null stat and return early:
 *
 *   craftItem   only the 39 block ids that are a CraftingManager recipe or
 *               FurnaceRecipes smelting output (CW_BLOCK_CRAFT_OUTPUT_ROWS,
 *               StatList.java:50-59), plus the four an alias row then reaches
 *               (86, 62, 75, 43).
 *   breakItem   NO block id at all: func_25149_b registers only
 *               `Item.itemsList[l].isDamagable()` (StatList.java:134) and
 *               `isDamagable()` is `maxDamage > 0 && !hasSubtypes`
 *               (Item.java:115-118), which no block item ever is.
 *   all four    only where Beta has a block: 1..96. Air (0) and 97..255 have
 *               none -- and CaveWii's own net-new blocks (97 tree2d, 200 iron
 *               chest) therefore get NO Beta statistic (lead ruling; a later
 *               card may add CaveWii-owned ids on top).
 *
 * AN ITEM ID (>= 256) has no alias -- no replaceSimilarBlocks row names one --
 * but it IS gated, per array, exactly as a block is (R2 MED 8; item ids used
 * to be waved through):
 *
 *   craftItem   only the 82 item ids that are a CraftingManager recipe or
 *               FurnaceRecipes smelting output (CW_ITEM_CRAFT_OUTPUT_ROWS,
 *               StatList.java:50-59).
 *   breakItem   only the 48 items with `maxDamage > 0 && !hasSubtypes`
 *               (CW_ITEM_DAMAGABLE_ROWS, StatList.java:134 /
 *               Item.java:115-118) -- tools, swords, hoes, armour,
 *               flint-and-steel, fishing rod, shears. NOT the bow.
 *   useItem     every item id (func_25155_a tests nothing but non-null).
 *   mineBlock   none: func_25153_a builds a 256-entry array.
 *
 * So this returns `id` unchanged for an item id its array registers, and
 * CW_STAT_BLOCK_NONE for one it does not. Callers MUST treat
 * CW_STAT_BLOCK_NONE as "apply nothing": it is deliberately outside the
 * 0..255 range so an unguarded array index cannot survive. */
uint16_t stat_alias_block_in(enum cw_stat_array array, uint16_t id);

/* stat_alias_block_in(CW_STAT_ARRAY_USE, id) -- the useItem fold, the plain
 * table direction. Kept as the short name because most callers mean exactly
 * that. NOT a substitute for the break array any more: breakItem has no block
 * counter at all, so that one has to name its own array. */
uint16_t stat_alias_block(uint16_t id);

/* Row index of a general stat id in CW_STAT_ROWS order, or -1 if `id` is not
 * a general stat (an array-counter id is not one). */
int stat_general_index(uint32_t id);

/* Inverse of stat_general_index(); 0 for an out-of-range index. */
uint32_t stat_general_id(int index);

/* Beta: unlocked == the totals map holds a count > 0 [design 1.2 point 2]. */
bool ach_unlocked(const struct stats_profile* p, int i);

/* Beta's parent gate: no parent, or the parent is unlocked [design 1.2
 * point 1]. An out-of-range index is never unlockable. */
bool ach_can_unlock(const struct stats_profile* p, int i);

/* Table accessors (the grid in lane C and B2's rules read these rather than
 * re-expanding the X-macro). Parent is -1 for openInventory. */
int ach_parent(int i);
uint16_t ach_icon_item(int i);
bool ach_is_special(int i);
bool ach_is_independent(int i);
const char* ach_key(int i);
/* Beta's displayColumn / displayRow; both signed, both negative for some
 * rows. 0 for an out-of-range index. */
int8_t ach_column(int i);
int8_t ach_row(int i);

#endif /* CW_ACHIEVEMENTS_STATE_H */
