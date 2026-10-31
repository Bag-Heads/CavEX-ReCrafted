/* Copyright (c) 2026 CaveWii_v2 contributors */
/*
 * The public notice / screen-data / status API -- brief B4.
 *
 * Spec: docs/achievements_design.md section 7. Every function here is a THIN
 * accessor over the state B1 (source/game/achievements/achievements_state.c,
 * achievements_persist.c) and B2 (achievements.c) already own: no counters,
 * no event handling, no file I/O and no drawing. What it adds is the shape
 * the screens want -- owner-explicit queries, the local/remote/total triple,
 * Beta's display coordinates and Beta's three value formatters.
 *
 * NO STATE AT ALL (R1 LOW 11/14). This file used to keep a per-slot ack
 * WATERMARK, because B2 exposed no notice mutator: peek() suppressed a notice
 * whose generation, profile, notices_queued and fields all still matched the
 * acked one. Two things were wrong with it. The watermark was never cleared on
 * init or bind, so a fresh module could start with a stale ack armed; and the
 * openInventory hint was re-queued as a notice every tick, which bumped
 * notices_queued and re-stamped started_ms 20 times a second, so the watermark
 * could never match the thing a player had actually acknowledged. B2 now owns
 * both halves -- achievements_notice_clear() clears the taken toast, and the
 * hint is the persistent `hint_active` flag rather than a notice -- and this
 * file is once again a pure accessor.
 */

#include "achievements_api.h"

#include <stdio.h>
#include <string.h>

/* ---- static tables ------------------------------------------------------
 * All three are EXPANDED FROM B1's X-macros, never retyped, so a table edit
 * in stats_table.h cannot drift from what the screens display. */

static const char* const s_ach_name_key[CW_ACH_COUNT] = {
#define CW_ACH(i, key, parent, icon, special, independent, column, row)        \
	"achievement." key,
	CW_ACH_ROWS
#undef CW_ACH
};

static const char* const s_ach_desc_key[CW_ACH_COUNT] = {
#define CW_ACH(i, key, parent, icon, special, independent, column, row)        \
	"achievement." key ".desc",
	CW_ACH_ROWS
#undef CW_ACH
};

static const char* const s_stat_key[CW_STAT_COUNT] = {
#define CW_STAT(id, key, type, owner) key,
	CW_STAT_ROWS
#undef CW_STAT
};

static const char* const s_stat_name_key[CW_STAT_COUNT] = {
#define CW_STAT(id, key, type, owner) "stat." key,
	CW_STAT_ROWS
#undef CW_STAT
};

static const uint8_t s_stat_type[CW_STAT_COUNT] = {
#define CW_STAT(id, key, type, owner) (uint8_t)CW_STAT_TYPE_##type,
	CW_STAT_ROWS
#undef CW_STAT
};

static const uint8_t s_stat_owner[CW_STAT_COUNT] = {
#define CW_STAT(id, key, type, owner) (uint8_t)CW_STAT_OWNER_##owner,
	CW_STAT_ROWS
#undef CW_STAT
};

/* Beta's displayColumn / displayRow are read through ach_column()/ach_row(),
 * which expand the SAME CW_ACH_ROWS table every other field comes from (R1
 * lead item 3). They used to be a parallel array here, cited from
 * `AchievementList.<clinit>`'s `new Achievement(id, name, displayColumn,
 * displayRow, icon, parent)`; a parallel array is exactly how a seventeenth
 * achievement would have silently taken openInventory's coordinates. The
 * citation now lives beside the values, in stats_table.h. */
_Static_assert(CW_ACH_COUNT == 16,
			   "AchievementList registers 16; CW_ACH_ROWS lists 16 rows in "
			   "that same registration order");

/* ---- shared plumbing ---------------------------------------------------- */

/* The one gate every screen query goes through: a valid slot, an initialised
 * module and something actually bound. Returns NULL otherwise -- never
 * another slot's profile. */
static const struct stats_profile* bound_profile(uint8_t slot) {
	struct ach_status st;
	if(slot >= CW_ACH_SLOT_COUNT)
		return NULL;
	memset(&st, 0, sizeof(st));
	if(!achievements_debug_status(slot, &st) || !st.bound)
		return NULL;
	return achievements_debug_profile(slot);
}

static void cell_of(struct stat_cell* out, const struct stats_pair* p) {
	out->local = p->local;
	out->remote = p->remote;
	out->total = stats_counter_total(p);
}

static void cell_zero(struct stat_cell* out) {
	if(out)
		memset(out, 0, sizeof(*out));
}

/* ---- the unlock notice -------------------------------------------------- */

bool achievements_notice_peek(uint8_t slot, struct ach_notice* out) {
	struct ach_status st;
	struct ach_notice n;
	bool has_toast;

	if(out) {
		memset(out, 0, sizeof(*out));
		out->ach_id = -1;
	}
	if(!out || slot >= CW_ACH_SLOT_COUNT)
		return false;
	memset(&st, 0, sizeof(st));
	if(!achievements_debug_status(slot, &st) || !st.bound)
		return false;
	memset(&n, 0, sizeof(n));
	/* False when the slot's FIFO is empty (ach_id < 0) -- but `n`
	 * still carries hint_active, which is a property of the profile and not of
	 * a toast, so it is copied out either way [design 7, R1 LOW 11]. */
	has_toast = achievements_notice_front(slot, &n);
	*out = n;
	return has_toast;
}

void achievements_notice_ack(uint8_t slot) {
	/* Pop exactly one front notice. An empty FIFO is a no-op; the openInventory
	 * hint is deliberately untouched -- it stays up until the achievement
	 * unlocks (R1 LOW 11/14). */
	achievements_notice_clear(slot);
}

/* ---- the achievement grid ----------------------------------------------- */

int achievements_count(void) {
	return CW_ACH_COUNT;
}

bool achievements_get(uint8_t slot, int i, struct ach_desc* out) {
	const struct stats_profile* p;

	if(out) {
		memset(out, 0, sizeof(*out));
		out->id = -1;
		out->parent = -1;
	}
	if(!out || i < 0 || i >= CW_ACH_COUNT)
		return false;
	p = bound_profile(slot);
	if(!p)
		return false;

	out->id = (int16_t)i;
	out->name_key = s_ach_name_key[i];
	out->desc_key = s_ach_desc_key[i];
	out->column = ach_column(i);
	out->row = ach_row(i);
	out->icon_item = ach_icon_item(i);
	out->parent = (int8_t)ach_parent(i);
	out->special = ach_is_special(i);
	out->unlocked = ach_unlocked(p, i);
	out->can_unlock = ach_can_unlock(p, i);
	return true;
}

/* ---- the statistics tabs ------------------------------------------------ */

bool stats_general_iter(uint8_t slot, int index, struct stat_row* out) {
	const struct stats_profile* p;

	if(out)
		memset(out, 0, sizeof(*out));
	if(!out || index < 0 || index >= CW_STAT_COUNT)
		return false;
	p = bound_profile(slot);
	if(!p)
		return false;

	out->id = stat_general_id(index);
	out->key = s_stat_key[index];
	out->name_key = s_stat_name_key[index];
	out->type = s_stat_type[index];
	out->owner = s_stat_owner[index];
	cell_of(&out->value, &p->general[index]);
	return true;
}

bool stats_block_row(uint8_t slot, uint16_t block_id, struct stat_cell* mined,
					 struct stat_cell* crafted, struct stat_cell* used) {
	const struct stats_profile* p;
	uint16_t canonical, aliased;

	cell_zero(mined);
	cell_zero(crafted);
	cell_zero(used);
	if(block_id >= CW_STAT_BLOCK_COUNT)
		return false;
	p = bound_profile(slot);
	if(!p)
		return false;

	/* Mined lives at the canonical id [design 2]; a disableStats() block has
	 * no counter at all and keeps the zeros written above. */
	canonical = stat_canonical_block(block_id);
	if(mined && canonical != CW_STAT_BLOCK_NONE)
		cell_of(mined, &p->mine[canonical]);
	/* Crafted / used are read through the SAME alias fold the counters are
	 * written with (R1 lead item 2): Beta's replaceAllSimilarBlocks runs over
	 * all four arrays, so grass reports dirt's craftItem here exactly as it
	 * reports dirt's mineBlock above. It is the ALIAS fold, not the canonical
	 * one -- a disableStats() block still has craft/use counters -- and it is
	 * PER ARRAY: craftItem folds two pairs the other way round (B3), so
	 * reading the crafted column with the use fold would show a pumpkin row
	 * an empty counter while the jack-o-lantern row held the count. The bound
	 * test is doing TWO jobs: every block id is below ITEMS_MAX, and
	 * CW_STAT_BLOCK_NONE (0xFFFF, "Beta registers no counter here at all" --
	 * an uncraftable block, air, a CaveWii-only id) is deliberately above it,
	 * so a gated cell keeps the zeros written at the top. See
	 * stat_alias_block_in(). */
	aliased = stat_alias_block_in(CW_STAT_ARRAY_CRAFT, block_id);
	if(crafted && aliased < ITEMS_MAX)
		cell_of(crafted, &p->craft[aliased]);
	aliased = stat_alias_block_in(CW_STAT_ARRAY_USE, block_id);
	if(used && aliased < ITEMS_MAX)
		cell_of(used, &p->use[aliased]);
	return true;
}

bool stats_item_row(uint8_t slot, uint16_t item_id, struct stat_cell* used,
					struct stat_cell* depleted, struct stat_cell* crafted) {
	const struct stats_profile* p;

	cell_zero(used);
	cell_zero(depleted);
	cell_zero(crafted);
	if(item_id >= ITEMS_MAX)
		return false;
	p = bound_profile(slot);
	if(!p)
		return false;

	if(used)
		cell_of(used, &p->use[item_id]);
	if(depleted)
		cell_of(depleted, &p->brk[item_id]);
	if(crafted)
		cell_of(crafted, &p->craft[item_id]);
	return true;
}

/* ---- Beta's three value formatters -------------------------------------- */

/* NumberFormat.getIntegerInstance(Locale.US): groups of three, comma
 * separated, no decimals. Writes at most 13 characters plus the
 * terminator. */
static bool fmt_grouped(uint32_t v, char* buf, size_t n) {
	char digits[11]; /* 4294967295 */
	int nd = 0;
	size_t need;
	size_t w = 0;
	int i;

	do {
		digits[nd++] = (char)('0' + (int)(v % 10u));
		v /= 10u;
	} while(v != 0u);

	need = (size_t)nd + (size_t)((nd - 1) / 3) + 1u;
	if(!buf || n == 0)
		return false;
	if(need > n) {
		buf[0] = '\0';
		return false;
	}
	for(i = nd - 1; i >= 0; i--) {
		buf[w++] = digits[i];
		if(i > 0 && (i % 3) == 0)
			buf[w++] = ',';
	}
	buf[w] = '\0';
	return true;
}

/* DecimalFormat("########0.00") + a space + the unit. The pattern has no
 * grouping and no maximum-integer-digit cap for a non-exponential pattern,
 * so "%.2f" is the same string. */
static bool fmt_fixed2(double v, const char* unit, char* buf, size_t n) {
	char num[64];
	int k;

	if(!buf || n == 0)
		return false;
	k = snprintf(num, sizeof(num), "%.2f", v);
	if(k < 0 || (size_t)k >= sizeof(num)) {
		buf[0] = '\0';
		return false;
	}
	k = snprintf(buf, n, "%s %s", num, unit);
	return k >= 0 && (size_t)k < n;
}

/* Beta's seconds branch appends the raw double, so the string keeps Java's
 * Double.toString shape. It is only reachable for v <= 600 ticks (above that
 * the minutes branch wins), and v/20 is then exactly v*5 hundredths, so the
 * shortest round-tripping decimal is that value with its trailing zero
 * dropped -- and Double.toString always keeps at least one fractional digit
 * ("1.0", not "1"). */
static bool fmt_seconds(uint32_t ticks, char* buf, size_t n) {
	uint32_t hundredths = ticks * 5u;
	uint32_t whole = hundredths / 100u;
	uint32_t frac = hundredths % 100u;
	int k;

	if(!buf || n == 0)
		return false;
	if((frac % 10u) == 0u)
		k = snprintf(buf, n, "%u.%u s", (unsigned)whole, (unsigned)(frac / 10u));
	else
		k = snprintf(buf, n, "%u.%02u s", (unsigned)whole, (unsigned)frac);
	return k >= 0 && (size_t)k < n;
}

bool stats_format_value(uint8_t type, uint32_t value, char* buf, size_t n) {
	if(!buf || n == 0)
		return false;
	buf[0] = '\0';

	switch((enum cw_stat_type)type) {
		case CW_STAT_TYPE_SIMPLE: return fmt_grouped(value, buf, n);
		case CW_STAT_TYPE_TIME: {
			/* StatTypeTime: seconds, minutes, hours, days, years -- the
			 * biggest unit STRICTLY greater than 0.5 wins. */
			double sec = (double)value / 20.0;
			double min = sec / 60.0;
			double hour = min / 60.0;
			double day = hour / 24.0;
			double year = day / 365.0;
			if(year > 0.5)
				return fmt_fixed2(year, "y", buf, n);
			if(day > 0.5)
				return fmt_fixed2(day, "d", buf, n);
			if(hour > 0.5)
				return fmt_fixed2(hour, "h", buf, n);
			if(min > 0.5)
				return fmt_fixed2(min, "m", buf, n);
			return fmt_seconds(value, buf, n);
		}
		case CW_STAT_TYPE_DISTANCE: {
			/* StatTypeDistance: centimetres, metres, kilometres. */
			double m = (double)value / 100.0;
			double km = m / 1000.0;
			int k;
			if(km > 0.5)
				return fmt_fixed2(km, "km", buf, n);
			if(m > 0.5)
				return fmt_fixed2(m, "m", buf, n);
			/* Beta appends the raw int: no thousands separators here. */
			k = snprintf(buf, n, "%u cm", (unsigned)value);
			return k >= 0 && (size_t)k < n;
		}
		default: return false;
	}
}

/* ---- status ------------------------------------------------------------- */

bool achievements_status(uint8_t slot, struct ach_status* out) {
	if(out)
		memset(out, 0, sizeof(*out));
	if(!out || slot >= CW_ACH_SLOT_COUNT)
		return false;
	/* Unlike every other query this one does NOT gate on `bound`: reporting
	 * "nothing is bound on this slot" is its job. It fails only when the
	 * module has never been initialised, i.e. there is no status at all. */
	if(!achievements_debug_status(slot, out)) {
		memset(out, 0, sizeof(*out));
		/* Even with nothing to report, WHY there is nothing is reportable:
		 * a module whose init failed (no memory, a stats path that is not a
		 * directory) says so instead of looking like a fresh one (R1 MED 4,
		 * LOW 13). */
		out->init_status = (uint8_t)achievements_init_status();
		return false;
	}
	return true;
}
