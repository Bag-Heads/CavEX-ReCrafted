/* Copyright (c) 2026 CaveWii_v2 contributors */
/**
 * @file achievements_api.h
 * @brief The PUBLIC surface of the achievement / statistic module: the
 *        per-owner unlock notice, the screen data API and the status API.
 *
 * Spec: docs/achievements_design.md section 7 ("Per-owner notices in
 * split-screen and the UI data API"), with the row/field meanings from 1.1
 * (the sixteen achievements), 1.3 (the toast), 1.4 (the statistic ids) and
 * 1.6 (what the two screens draw).
 *
 * WHO CALLS WHAT
 *  - lane A (per-view render): achievements_notice_peek() /
 *    achievements_notice_ack() only. Everything else is screen data.
 *  - lane C (the achievement grid and the three statistics tabs):
 *    achievements_count(), achievements_get(), stats_general_iter(),
 *    stats_block_row(), stats_item_row(), stats_format_value(),
 *    achievements_status().
 *
 * THREADING: MAIN THREAD ONLY, every function. They read the same state the
 * drain (achievements_tick()) owns; game_event_emit() is the only
 * any-thread entry point in this module and it lives in game_event.h.
 *
 * OWNER-EXPLICIT, ALWAYS (Astra change a, design 7): every call takes the
 * owner `slot` as its FIRST parameter. There is no "current player". A view
 * passes ITS OWN owner slot and can therefore never render, or leak, the
 * other split-screen player's progress.
 *
 * TOTAL FOR AN UNBOUND SLOT: every function here is total. A slot outside
 * 0..CW_ACH_SLOT_COUNT-1, a slot with nothing bound, a NULL `out` or an
 * out-of-range index yields a DEFINED EMPTY result -- the `out` struct is
 * zeroed (ids set to -1 where -1 means "none") and the call returns false.
 * Nothing asserts, and no call ever falls back to another slot's state.
 *
 * WHAT THIS HEADER DOES NOT DO (by brief): it does not draw, it does not
 * sort, and it holds no counters of its own. The Blocks/Items tabs sort in
 * lane C with the documented deterministic rule (design 7: value desc, then
 * canonical id asc); this module hands out rows in table order.
 */

#ifndef CW_ACHIEVEMENTS_API_H
#define CW_ACHIEVEMENTS_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "achievements.h"		/* struct ach_notice, struct ach_status */
#include "achievements_state.h" /* struct stats_profile, the table accessors */
#include "stats_table.h"		/* enum cw_stat_type, enum cw_stat_owner */

/** Longest string stats_format_value() can produce, terminator included
 *  ("4,294,967,295" is 13, "42949672.95 km" is 14). */
#define CW_STAT_VALUE_MAX 32

/**
 * One counter as a screen reads it: the two provenance columns of design 6
 * plus the number Beta itself would have shown.
 *
 * `total` is local + remote computed in 64-bit and saturated back to 32
 * (stats_counter_total(), design 2) -- the DEFAULT a tab prints; `local` and
 * `remote` are there so lane C can offer the split without a second query.
 */
struct stat_cell {
	uint32_t local;	 /**< observed in a local world (single player / host). */
	uint32_t remote; /**< observed while joined to a remote server.        */
	uint32_t total;	 /**< saturated local + remote; what Beta shows.       */
};

/**
 * One row of the General tab [design 1.6]: a Beta general statistic id, how
 * to name it, how to format it, and its value.
 *
 * `key` and `name_key` point at static storage that outlives any caller;
 * they are never built into the caller's buffer.
 */
struct stat_row {
	uint32_t id;		  /**< the Beta stat id (1000..2025).             */
	const char* key;	  /**< bare Beta name, e.g. "walkOneCm".          */
	const char* name_key; /**< display key, "stat.<key>".                 */
	uint8_t type;		  /**< enum cw_stat_type: how to format `value`.  */
	uint8_t owner;		  /**< enum cw_stat_owner: Beta's client/server split. */
	struct stat_cell value;
};

/**
 * One cell of the achievement grid [design 1.6, 7]. Everything
 * GuiAchievements needs to place, label, frame and shade one achievement.
 *
 * `column`/`row` are Beta's `displayColumn`/`displayRow`
 * (AchievementList.<clinit>, `new Achievement(id, name, column, row, icon,
 * parent)`); both are SIGNED and both go negative (makeBread is at -1/-3,
 * bakeCake at 0/-5), so the grid origin is the openInventory cell at 0/0 and
 * lane C offsets by the min column/row it finds.
 *
 * Shading, per design 1.6: `unlocked` -> full colour; else `can_unlock`
 * (the parent is unlocked) -> dimmed; else dark. `special` -> the special
 * frame. A locked row still carries its keys and icon so the hover text can
 * read "Requires '<parent>'".
 */
struct ach_desc {
	int16_t id;			  /**< 0..CW_ACH_COUNT-1, or -1 on a failed query. */
	const char* name_key; /**< "achievement.<key>"; static storage.        */
	const char* desc_key; /**< "achievement.<key>.desc"; static storage.    */
	int8_t column;		  /**< Beta displayColumn, -1..8.                  */
	int8_t row;			  /**< Beta displayRow, -5..6.                     */
	uint16_t icon_item;	  /**< Beta item/block id drawn in the cell.       */
	int8_t parent;		  /**< prerequisite index, or -1 (openInventory).  */
	bool special;		  /**< setSpecial(): onARail and flyPig.           */
	bool unlocked;		  /**< this owner has it (count > 0) [design 1.2]. */
	bool can_unlock;	  /**< no parent, or the parent is unlocked.       */
};

/* ---- the unlock notice (lane A, per-view render) ------------------------ */

/**
 * Read `slot`'s current toast [design 1.3, 7].
 *
 * Caller: lane A's PER-VIEW render, with the view's OWN owner slot. A notice
 * belongs to the slot whose actor earned it, so the other view draws
 * nothing -- that is the whole point of the owner-explicit signature.
 *
 * Thread: main only.
 *
 * Peek returns the front of a per-owner FIFO, in unlock order. `started_ms`
 * records enqueue time, not presentation time. The presenter owns the 3.0 s
 * visible window and freezes it behind menus, full map and death (Brief E2).
 *
 * `out->hint_active` is the openInventory HINT, and it is NOT a toast (R1 LOW
 * 11): it is a persistent flag, true for as long as that achievement is
 * locked for this owner, and it is filled in for any bound slot WHATEVER THIS
 * CALL RETURNS. So a view draws both with one query:
 *
 *	 struct ach_notice n;
 *	 if(achievements_notice_peek(slot, &n)) draw_toast(n.ach_id, n.started_ms);
 *	 else if(n.hint_active)                 draw_open_inventory_hint();
 *
 * Lane C looks the bound control name up from the CURRENT VIEW's owner,
 * bindings and controller (Astra #9) -- this module never names a button.
 *
 * @param slot owner slot, 0..CW_ACH_SLOT_COUNT-1.
 * @param out  receives the notice; always written (zeroed, ach_id = -1, when
 *             the call returns false -- `hint_active` is still filled in for a
 *             valid bound slot).
 * @return true when there IS a TOAST to draw for this slot; false for a bad
 *         slot, an unbound slot, a NULL `out`, or an empty/acknowledged toast.
 *         An active hint alone does NOT make it true.
 */
bool achievements_notice_peek(uint8_t slot, struct ach_notice* out);

/**
 * Acknowledge `slot`'s front toast: pop exactly one; the next peek returns
 * its successor, if any (Brief E2).
 *
 * Caller: lane A, once the view has finished with the toast (its window
 * elapsed, or the view was torn down). Thread: main only.
 *
 * A no-op for a bad slot or a slot with no toast. It is a direct call to the
 * module's achievements_notice_clear() and keeps no state of its own (R1 LOW
 * 11/14): acknowledging pops the one toast that was shown. The openInventory
 * HINT is not a toast and is NOT cleared by this -- it stays up until the
 * achievement unlocks, which is what Beta does too.
 */
void achievements_notice_ack(uint8_t slot);

/* ---- the achievement grid (lane C) -------------------------------------- */

/**
 * How many achievements the grid has: Beta registers 16 [design 1.1].
 * Constant, slot-independent, always safe to call. Caller: lane C. Thread:
 * main only (it touches nothing, but keep every call on one thread).
 * @return CW_ACH_COUNT.
 */
int achievements_count(void);

/**
 * Fill `out` with one achievement's grid data for one owner [design 7].
 *
 * Caller: lane C's GuiAchievements, iterating `i` over
 * 0..achievements_count()-1 for the screen's owner slot. Thread: main only.
 *
 * Rows come back in Beta's REGISTRATION order (index i is Beta's
 * achievement i, stat id CW_ACH_BASE + i), which is also topological over
 * the parent graph, so a parent is always described before its children.
 * Nothing here sorts or lays out -- `column`/`row` are the raw Beta
 * coordinates and lane C owns the grid transform.
 *
 * @param slot owner slot.
 * @param i    achievement index, 0..CW_ACH_COUNT-1.
 * @param out  receives the row; always written (zeroed with id = -1 and
 *             parent = -1 when the call returns false).
 * @return true on success; false for a bad slot, an UNBOUND slot (a player
 *         with no progress tracking has no grid to draw), an index out of
 *         range or a NULL `out`.
 */
bool achievements_get(uint8_t slot, int i, struct ach_desc* out);

/* ---- the statistics tabs (lane C) --------------------------------------- */

/**
 * Iterate the General tab [design 1.6]: `index` 0.. walks the Beta general
 * statistics in stats_table.h order (1000..2025) and returns false once past
 * the last one, so a caller loops `while(stats_general_iter(slot, i++, &r))`
 * without knowing the count.
 *
 * Caller: lane C's GuiStats General tab. Thread: main only.
 *
 * The per-id arrays (mineBlock / craftItem / useItem / breakItem) are NOT
 * part of this iteration -- they are the Blocks and Items tabs, below.
 *
 * @param slot  owner slot.
 * @param index 0-based row index.
 * @param out   receives the row; always written (zeroed when false).
 * @return true while `index` names a row; false past the end, for a bad or
 *         unbound slot, or for a NULL `out`.
 */
bool stats_general_iter(uint8_t slot, int index, struct stat_row* out);

/**
 * One row of the Blocks tab [design 1.6]: Mined / Crafted / Used for one
 * block id, each with its local, remote and saturated total.
 *
 * Caller: lane C's GuiStats Blocks tab. Thread: main only.
 *
 * `mined` is read through the CANONICAL fold (stat_canonical_block(), design
 * 2 / StatList.replaceAllSimilarBlocks): grass and farmland report dirt's
 * counter, the lit furnace reports the furnace's, and so on, because that is
 * the single counter the module actually increments. A block whose Beta
 * registration was suppressed by Block.disableStats() (bedrock, water, lava,
 * leaves, ...) has no mine counter at all and reports zeros -- lane C uses
 * that to decide whether the row is worth a line.
 *
 * `crafted` and `used` are read through the SAME fold their counters are
 * written with, so read and write always name one bucket (R1 lead item 2).
 * Beta runs replaceAllSimilarBlocks over craftItem/useItem/breakItem as well
 * as mineBlock, so crafting a lit furnace counts under the furnace here too --
 * but through stat_alias_block_in(), each array's OWN registration + fold:
 * useItem is not built behind getEnableStats(), so a wooden door or a cake has
 * a use counter while it has no mine counter, and craftItem exists only for a
 * recipe/smelting output, so an uncraftable block reports zeros in `crafted`
 * because Beta has no such counter (F3). Lane C reads a zero cell the same way
 * whether the counter is empty or absent.
 *
 * Any of the three out-pointers may be NULL to skip that column.
 *
 * @param slot     owner slot.
 * @param block_id 0..CW_STAT_BLOCK_COUNT-1.
 * @return true on success; false (with every non-NULL cell zeroed) for a bad
 *         slot, an unbound slot or a block id out of range.
 */
bool stats_block_row(uint8_t slot, uint16_t block_id, struct stat_cell* mined,
					 struct stat_cell* crafted, struct stat_cell* used);

/**
 * One row of the Items tab [design 1.6]: Used / Depleted / Crafted for one
 * item id, each with its local, remote and saturated total. Column order
 * matches design 7's signature, which is the order the tab draws.
 *
 * Caller: lane C's GuiStats Items tab. Thread: main only. Any out-pointer
 * may be NULL to skip that column.
 *
 * @param slot    owner slot.
 * @param item_id 0..ITEMS_MAX-1.
 * @return true on success; false (with every non-NULL cell zeroed) for a bad
 *         slot, an unbound slot or an item id out of range.
 */
bool stats_item_row(uint8_t slot, uint16_t item_id, struct stat_cell* used,
					struct stat_cell* depleted, struct stat_cell* crafted);

/**
 * Format a counter the way Beta's stats screen prints it [design 1.6].
 *
 * Caller: lane C, for every value on the three tabs. Thread: main only (it
 * is pure, but keep one thread). Slot-independent: the value is the caller's.
 *
 * The three formatters are Beta's, reproduced string-for-string:
 *  - CW_STAT_TYPE_SIMPLE (StatTypeSimple):
 *    `NumberFormat.getIntegerInstance(Locale.US)` -> an integer with comma
 *    thousands separators: 0 -> "0", 1000 -> "1,000", 1234567 ->
 *    "1,234,567".
 *  - CW_STAT_TYPE_TIME (StatTypeTime), value in TICKS: seconds = v/20,
 *    then minutes, hours, days, years; the first unit whose value is
 *    STRICTLY > 0.5 wins, printed through DecimalFormat("########0.00") with
 *    a " y" / " d" / " h" / " m" suffix. Below that (v <= 600 ticks) Beta
 *    appends the raw double, so the seconds form keeps Java's
 *    Double.toString shape -- one fractional digit unless a second is
 *    needed: 0 -> "0.0 s", 10 -> "0.5 s", 25 -> "1.25 s", 600 -> "30.0 s",
 *    601 -> "0.50 m".
 *  - CW_STAT_TYPE_DISTANCE (StatTypeDistance), value in CENTIMETRES:
 *    m = v/100, km = m/1000; km wins if > 0.5, else m if > 0.5, else the raw
 *    integer centimetres with no separators: 50 -> "50 cm", 51 -> "0.51 m",
 *    50000 -> "500.00 m", 50001 -> "0.50 km".
 *
 * Two documented deviations, both from CaveWii's saturating uint32 counters
 * (design 2/8) rather than from the formatters: a value above INT32_MAX
 * prints as the unsigned number Beta's wrapping int could not have reached,
 * and the DecimalFormat tie rule is C's round-half-to-even on the binary
 * value rather than Java's on the shortest decimal -- reachable only for a
 * value whose scaled form is EXACTLY x.xx5, which no counter in the table
 * can produce.
 *
 * @param type  enum cw_stat_type.
 * @param value the counter (ticks for TIME, centimetres for DISTANCE).
 * @param buf   destination; always NUL-terminated when `n` > 0.
 * @param n     size of `buf`; CW_STAT_VALUE_MAX is always enough.
 * @return true when the whole string fit; false for a NULL/zero-size buffer,
 *         an unknown `type` (buf gets ""), or a truncated result.
 */
bool stats_format_value(uint8_t type, uint32_t value, char* buf, size_t n);

/* ---- status (lane C, and any diagnostic overlay) ------------------------ */

/**
 * The owner's module status [design 7]: whether anything is bound, the mode
 * and generation it is bound at, the load result and save-failure history
 * from the persistence layer, the per-reason rejection counters, and
 * `progress_incomplete` (the coalescer ran out of keys, or an ORDERED event
 * was rejected with ACH_REJECT_OVERFLOW because both rings were full -- either
 * way counts were lost and the module says so rather than approximating).
 *
 * Caller: lane C, to tell the player the truth instead of silently showing
 * zeros -- "no progress tracking for this player" when `bound` is false, "a
 * save is failing" when `save_failures` is non-zero, "some progress was
 * lost" when `progress_incomplete` is set. Thread: main only.
 *
 * This is the ONE call that stays meaningful for an unbound slot, because
 * "unbound" is exactly what it has to be able to report: it returns true
 * with `bound == false` and every counter zeroed. It returns false only when
 * there is nothing to report at all -- a slot out of range, a NULL `out`, or
 * a module that has never been through achievements_init().
 *
 * @param slot owner slot.
 * @param out  receives the status; always written (zeroed when false).
 * @return true when `out` holds this slot's status.
 */
bool achievements_status(uint8_t slot, struct ach_status* out);

#endif /* CW_ACHIEVEMENTS_API_H */
