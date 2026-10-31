/* Copyright (c) 2026 CaveWii_v2 contributors */
/* Per-profile achievement / statistic state [docs/achievements_design.md 2].
 * Data + arithmetic only; events, lifecycle and notices are B2/B4. */

#include <assert.h>
#include <string.h>

#include "achievements_state.h"

void stats_profile_reset(struct stats_profile* p, const char* profile_id) {
	assert(p);
	memset(p, 0, sizeof(*p));
	/* NEVER TRUNCATE (R1 MED 3). Truncating a 17-character key left the
	 * profile holding a 16-character key that stats_profile_save() would
	 * happily turn into a path, while stats_profile_load() -- which builds its
	 * path from the RAW key -- could never open that file: one key, two files,
	 * and the identity check then quarantining the file the same session had
	 * just written. An invalid key simply produces no key at all, and the save
	 * layer's own STATS_SAVE_ERR_BAD_ARG is what the caller sees. */
	if(stats_profile_id_valid(profile_id))
		memcpy(p->profile_id, profile_id, strlen(profile_id) + 1);
}

bool stats_profile_id_valid(const char* profile_id) {
	if(!profile_id || !profile_id[0])
		return false;
	size_t n = strlen(profile_id);
	if(n > CW_PROFILE_ID_MAX)
		return false;
	for(size_t i = 0; i < n; i++) {
		char c = profile_id[i];
		if(!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
			return false;
	}
	return true;
}

void stats_counter_add(struct stats_pair* p, enum stats_column column,
					   uint32_t amount) {
	assert(p);
	if(amount == 0)
		return;
	uint32_t* slot = (column == STATS_COLUMN_REMOTE) ? &p->remote : &p->local;
	/* Saturate instead of wrapping [design 2]. Written as a headroom test so
	 * no intermediate ever overflows. */
	if(*slot > UINT32_MAX - amount)
		*slot = UINT32_MAX;
	else
		*slot += amount;
}

uint32_t stats_counter_total(const struct stats_pair* p) {
	assert(p);
	uint64_t total = (uint64_t)p->local + (uint64_t)p->remote;
	return (total > (uint64_t)UINT32_MAX) ? UINT32_MAX : (uint32_t)total;
}

/* ---- Beta's per-array REGISTRATION gate [StatList, F3] -------------------
 * "Does Beta create a StatBase for this block id in this array at all?" --
 * the question every one of the four registration loops asks before
 * replaceAllSimilarBlocks() ever runs, and the one CaveWii used to skip for
 * the three item arrays (the deviation achievements_state.h recorded).
 *
 * Common to all four: a Beta block must exist at `id`. func_25153_a tests
 * `Block.blocksList[j] != null` (StatList.java:86); the other three test
 * `Item.itemsList[l] != null` (StatList.java:70 / 108 / 134), which
 * Block.java:863-869 fills with an ItemBlock for exactly the non-null blocks,
 * so the two spellings select the same ids: CW_BLOCK_BETA_FIRST..LAST. */
static bool block_registered_in(enum cw_stat_array array, uint16_t id) {
	if(id < CW_BLOCK_BETA_FIRST || id > CW_BLOCK_BETA_LAST)
		return false;

	switch(array) {
		case CW_STAT_ARRAY_MINE:
			/* func_25153_a's second test, `getEnableStats()`
			 * (StatList.java:86): the twenty disableStats() blocks. */
#define CW_BLOCK_NO_STAT(b)                                                    \
	if(id == (b))                                                              \
		return false;
			CW_BLOCK_NO_STAT_ROWS
#undef CW_BLOCK_NO_STAT
			return true;

		case CW_STAT_ARRAY_CRAFT:
			/* func_25157_c registers ONLY the HashSet of CraftingManager
			 * recipe outputs and FurnaceRecipes smelting outputs
			 * (StatList.java:50-59, registered at :75). */
#define CW_BLOCK_CRAFT_OUTPUT(b)                                               \
	if(id == (b))                                                              \
		return true;
			CW_BLOCK_CRAFT_OUTPUT_ROWS
#undef CW_BLOCK_CRAFT_OUTPUT
			return false;

		case CW_STAT_ARRAY_USE:
			/* func_25155_a registers every non-null Item.itemsList entry and
			 * does NOT consult getEnableStats() (StatList.java:106-120), so a
			 * door, sign, cake or repeater has a useItem counter with no
			 * mineBlock one. */
			return true;

		case CW_STAT_ARRAY_BREAK:
			/* func_25149_b registers only `Item.itemsList[l].isDamagable()`
			 * (StatList.java:134), which is `maxDamage > 0 && !hasSubtypes`
			 * (Item.java:115-118). Every block item is an ItemBlock (or
			 * ItemCloth/Log/Slab/Sapling/Leaves/Piston), none of which ever
			 * sets a maxDamage -- the five that touch it call setMaxDamage(0)
			 * explicitly -- so Beta registers NOTHING in 0..255 and the whole
			 * breakItem column of the golden is "-". A block cannot be
			 * depleted, so no well-formed GE_ITEM_BROKEN names one. */
			return false;

		default: return false;
	}
}

/* ---- Beta per-array REGISTRATION gate for ITEM ids [StatList, R2 MED 8] --
 * The same question as block_registered_in(), for the ids at or above 256 --
 * which the module used to answer "yes, always", so a craft of an apple and a
 * break of a snowball both reached counters Beta never registers.
 *
 *   craftItem   func_25157_c registers the HashSet of CraftingManager recipe
 *               outputs and FurnaceRecipes smelting outputs and NOTHING else
 *               (StatList.java:50-59, registered at :75).
 *   useItem     func_25155_a registers every non-null Item.itemsList entry
 *               with no further test (StatList.java:108), so every item id
 *               has a use counter.
 *   breakItem   func_25149_b registers only `isDamagable()`
 *               (StatList.java:134) = `maxDamage > 0 && !hasSubtypes`
 *               (Item.java:115-118): the 48 tools, swords, hoes, armour
 *               pieces, flint-and-steel, fishing rod and shears.
 *   mineBlock   there is no mineBlock entry above 255 at all: func_25153_a
 *               builds a `new StatBase[256]` (StatList.java:83).
 *
 * The two sets are CW_ITEM_CRAFT_OUTPUT_ROWS and CW_ITEM_DAMAGABLE_ROWS in
 * stats_table.h, each row carrying its decompile citation, and the golden
 * tests/golden/achievements/b4_item_registration.txt pins both. No alias row
 * names an item id, so an item never folds: it keeps its own counter or it
 * has none. */
static bool item_registered_in(enum cw_stat_array array, uint16_t id) {
	switch(array) {
		case CW_STAT_ARRAY_MINE: return false;

		case CW_STAT_ARRAY_CRAFT:
#define CW_ITEM_CRAFT_OUTPUT(i)                                                \
	if(id == (i))                                                              \
		return true;
			CW_ITEM_CRAFT_OUTPUT_ROWS
#undef CW_ITEM_CRAFT_OUTPUT
			return false;

		case CW_STAT_ARRAY_USE: return true;

		case CW_STAT_ARRAY_BREAK:
#define CW_ITEM_DAMAGABLE(i)                                                   \
	if(id == (i))                                                              \
		return true;
			CW_ITEM_DAMAGABLE_ROWS
#undef CW_ITEM_DAMAGABLE
			return false;

		default: return false;
	}
}

/* The last cell an id was assigned during the replay below, or the id itself
 * (its own cell) if no row has written it yet. Read backwards: a row may
 * overwrite a cell an earlier row already wrote. */
static uint16_t alias_cell_of(const uint16_t* ov_id, const uint16_t* ov_cell,
							  int n, uint16_t id) {
	for(int k = n - 1; k >= 0; k--)
		if(ov_id[k] == id)
			return ov_cell[k];
	return id;
}

/* StatList.replaceAllSimilarBlocks() replayed over ONE id, in an array whose
 * registration set is block_registered_in()'s.
 *
 * replaceSimilarBlocks(a, i, j) (StatList.java:162-176) is:
 *
 *	 if(a[i] != null && a[j] == null) { a[j] = a[i]; }   // j takes i's
 *	 else                             { a[i] = a[j]; }   // i takes j's
 *
 * so after a row BOTH ids hold the same array cell -- `i`'s when the
 * inverting branch runs, `j`'s otherwise -- and that cell is null (no counter
 * at all) unless the id it came from was registered. The counter's own id is
 * the id of the cell that registered it, which is what this returns.
 *
 * The eleven rows are replayed IN ORDER, against the running assignment,
 * because Beta's is a sequence of array writes and two rows share the target
 * cell 3: (2,3) can write a[3] before (60,3) reads it. Each row writes
 * exactly one cell, so CW_BLOCK_ALIAS_COUNT overrides bound the whole replay
 * and it needs no 256-cell array (nor any cached state, which would need a
 * lock -- B2 calls this under its own).
 *
 * This is also where the two craftItem inversions now come from: with the
 * craft registration set in hand, `a[91] != null && a[86] == null` is simply
 * TRUE, so the branch that CW_BLOCK_CRAFT_INVERT_ROWS used to hard-code falls
 * out of the same code every other pair takes. Those rows stay in
 * stats_table.h as the cited expectation, and test_stats_counters pins the
 * derived fold against them. */
uint16_t stat_alias_block_in(enum cw_stat_array array, uint16_t id) {
	uint16_t ov_id[CW_BLOCK_ALIAS_COUNT];
	uint16_t ov_cell[CW_BLOCK_ALIAS_COUNT];
	int n = 0;
	uint16_t cell;

	if(id >= CW_STAT_BLOCK_COUNT) {
		/* An item id has no alias -- no replaceSimilarBlocks row names one --
		 * but it does have a per-array REGISTRATION gate (R2 MED 8). */
		return item_registered_in(array, id) ? id : CW_STAT_BLOCK_NONE;
	}

	/* A row only ever writes a cell of an id it NAMES, so an id no row names
	 * keeps its own cell and the replay cannot move it. Skipping it there is
	 * bit-exact by construction (and the golden checks all 4 x 256 cells);
	 * it keeps the common case at eleven compares plus one registration
	 * test instead of twenty-two registration tests. */
	{
		bool in_a_row = false;
#define CW_BLOCK_ALIAS(from, to)                                               \
	if(id == (from) || id == (to))                                             \
		in_a_row = true;
		CW_BLOCK_ALIAS_ROWS
#undef CW_BLOCK_ALIAS
		if(!in_a_row)
			return block_registered_in(array, id) ? id : CW_STAT_BLOCK_NONE;
	}

#define CW_BLOCK_ALIAS(from, to)                                               \
	{                                                                          \
		uint16_t ci = alias_cell_of(ov_id, ov_cell, n, (uint16_t)(from));      \
		uint16_t cj = alias_cell_of(ov_id, ov_cell, n, (uint16_t)(to));        \
		if(block_registered_in(array, ci)                                      \
		   && !block_registered_in(array, cj)) {                               \
			ov_id[n] = (uint16_t)(to);                                         \
			ov_cell[n] = ci;                                                   \
		} else {                                                               \
			ov_id[n] = (uint16_t)(from);                                       \
			ov_cell[n] = cj;                                                   \
		}                                                                      \
		n++;                                                                   \
	}
	CW_BLOCK_ALIAS_ROWS
#undef CW_BLOCK_ALIAS

	cell = alias_cell_of(ov_id, ov_cell, n, id);
	return block_registered_in(array, cell) ? cell : CW_STAT_BLOCK_NONE;
}

uint16_t stat_alias_block(uint16_t id) {
	return stat_alias_block_in(CW_STAT_ARRAY_USE, id);
}

uint16_t stat_canonical_block(uint16_t id) {
	/* The mine array is the same two Beta steps -- registration
	 * (func_25153_a's `blocksList[j] != null && getEnableStats()`), then the
	 * fold -- so it is the same function with CW_STAT_ARRAY_MINE, and the
	 * disableStats() set is just that array's registration gate. The one
	 * difference is the out-of-range contract: an ITEM id has no mineBlock
	 * counter (NONE), where in the item arrays it is simply not a block id
	 * and passes through unchanged. */
	if(id >= CW_STAT_BLOCK_COUNT)
		return CW_STAT_BLOCK_NONE;
	return stat_alias_block_in(CW_STAT_ARRAY_MINE, id);
}

int stat_general_index(uint32_t id) {
	int index = 0;
#define CW_STAT(sid, key, type, owner)                                         \
	if(id == (uint32_t)(sid))                                                  \
		return index;                                                          \
	index++;
	CW_STAT_ROWS
#undef CW_STAT
	(void)index;
	return -1;
}

uint32_t stat_general_id(int index) {
	int cursor = 0;
#define CW_STAT(sid, key, type, owner)                                         \
	if(index == cursor)                                                        \
		return (uint32_t)(sid);                                                \
	cursor++;
	CW_STAT_ROWS
#undef CW_STAT
	(void)cursor;
	return 0;
}

bool ach_unlocked(const struct stats_profile* p, int i) {
	assert(p);
	if(i < 0 || i >= CW_ACH_COUNT)
		return false;
	/* Beta: "unlocked" is simply count > 0 in the totals map. Both columns
	 * count -- an achievement earned on a remote server stays earned. */
	return p->ach[i].local > 0 || p->ach[i].remote > 0;
}

bool ach_can_unlock(const struct stats_profile* p, int i) {
	assert(p);
	if(i < 0 || i >= CW_ACH_COUNT)
		return false;
	int parent = ach_parent(i);
	return parent < 0 || ach_unlocked(p, parent);
}

int ach_parent(int i) {
#define CW_ACH(idx, key, parent, icon, special, independent, column, row)     \
	if(i == (idx))                                                             \
		return (parent);
	CW_ACH_ROWS
#undef CW_ACH
	return -1;
}

uint16_t ach_icon_item(int i) {
#define CW_ACH(idx, key, parent, icon, special, independent, column, row)     \
	if(i == (idx))                                                             \
		return (uint16_t)(icon);
	CW_ACH_ROWS
#undef CW_ACH
	return 0;
}

bool ach_is_special(int i) {
#define CW_ACH(idx, key, parent, icon, special, independent, column, row)     \
	if(i == (idx))                                                             \
		return (special) != 0;
	CW_ACH_ROWS
#undef CW_ACH
	return false;
}

bool ach_is_independent(int i) {
#define CW_ACH(idx, key, parent, icon, special, independent, column, row)     \
	if(i == (idx))                                                             \
		return (independent) != 0;
	CW_ACH_ROWS
#undef CW_ACH
	return false;
}

const char* ach_key(int i) {
#define CW_ACH(idx, key, parent, icon, special, independent, column, row)     \
	if(i == (idx))                                                             \
		return (key);
	CW_ACH_ROWS
#undef CW_ACH
	return "";
}

int8_t ach_column(int i) {
#define CW_ACH(idx, key, parent, icon, special, independent, column, row)     \
	if(i == (idx))                                                             \
		return (int8_t)(column);
	CW_ACH_ROWS
#undef CW_ACH
	return 0;
}

int8_t ach_row(int i) {
#define CW_ACH(idx, key, parent, icon, special, independent, column, row)     \
	if(i == (idx))                                                             \
		return (int8_t)(row);
	CW_ACH_ROWS
#undef CW_ACH
	return 0;
}
