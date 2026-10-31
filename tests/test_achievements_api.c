/* Copyright (c) 2026 CaveWii_v2 contributors */
/* Brief B4 / docs/achievements_design.md section 7: the PUBLIC notice,
 * screen-data and status API that lane A and lane C read.
 *
 * Asserts, with two bound slots and a scripted B2 event feed:
 *   - notice isolation per slot; that ack clears the taken toast without
 *     hiding the next one; and that the openInventory hint is a persistent
 *     flag rather than a toast (it survives an ack, never counts, and goes
 *     out only when the achievement unlocks);
 *   - achievements_get() fields for a locked row, a can-unlock row, an
 *     unlocked row and the two special rows, including Beta's signed
 *     display coordinates;
 *   - the three formatters against fixed expected strings (thousands
 *     separators, the seconds/minutes/hours/days/years ladder, the m/km
 *     switch);
 *   - every row is EMPTY for an unbound slot and for a slot that does not
 *     exist -- and never falls back to the other slot's state;
 *   - achievements_status() reports an injected save failure and an event
 *     rejection.
 *
 * The values themselves are B1/B2's business (test_achievements_persist,
 * _core, _owner, _seam); what is under test here is that this API hands the
 * screens the SAME state, per owner, in the shape design 7 promises.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "support/test_fixture_dir.h"

#include "game/achievements/achievements_api.h"

static char s_dir[128];
static int s_case;

static void make_dir(const char* dir) {
#ifdef _WIN32
	(void)mkdir(dir);
#else
	(void)mkdir(dir, 0777);
#endif
}

/* ---------------------------------------------------------------- helpers */

static struct game_event mk(uint8_t kind, uint8_t authority, uint8_t slot,
							uint64_t generation) {
	struct game_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.kind = kind;
	ev.authority = authority;
	ev.who.slot = slot;
	ev.who.generation = generation;
	return ev;
}

static void emit_plain(uint8_t kind, uint8_t authority, uint8_t slot,
					   uint64_t generation) {
	struct game_event ev = mk(kind, authority, slot, generation);
	game_event_emit(&ev);
}

static void emit_item(uint8_t kind, uint8_t authority, uint8_t slot,
					  uint64_t generation, uint16_t id, uint16_t count) {
	struct game_event ev = mk(kind, authority, slot, generation);
	ev.u.item.id = id;
	ev.u.item.count = count;
	game_event_emit(&ev);
}

/* Fresh profile keys per case so a re-init never reloads a previous case's
 * file, and so a case's own unlock state is the only thing under test. */
static void next_ids(char* a, char* b, size_t n) {
	snprintf(a, n, "api%da", s_case);
	snprintf(b, n, "api%db", s_case);
	s_case++;
}

static void expect_fmt(enum cw_stat_type type, uint32_t v, const char* want) {
	char buf[CW_STAT_VALUE_MAX];
	assert(stats_format_value((uint8_t)type, v, buf, sizeof(buf)));
	if(strcmp(buf, want) != 0) {
		/* stderr: an abort()ing assert never flushes stdout on Windows. */
		fprintf(stderr, "FAIL format type %d value %lu -> \"%s\", want \"%s\"\n",
				(int)type, (unsigned long)v, buf, want);
		assert(0);
	}
}

/* Row index of a general stat id in the iteration order. */
static int general_row(uint8_t slot, uint32_t id, struct stat_row* out) {
	int i = 0;
	while(stats_general_iter(slot, i, out)) {
		if(out->id == id)
			return i;
		i++;
	}
	return -1;
}

/* ------------------------------------------------------------------ cases */

/* Beta's StatTypeSimple / StatTypeTime / StatTypeDistance, string for
 * string. Every expectation below is the decompile's own arithmetic:
 * simple = NumberFormat.getIntegerInstance(Locale.US); time divides ticks by
 * 20/60/60/24/365 and takes the first unit STRICTLY > 0.5 through
 * DecimalFormat("########0.00"); distance divides centimetres by 100/1000 on
 * the same > 0.5 rule. */
static void test_format_values(void) {
	char buf[CW_STAT_VALUE_MAX];
	char tiny[4];

	/* SIMPLE: thousands separators, no decimals. */
	expect_fmt(CW_STAT_TYPE_SIMPLE, 0u, "0");
	expect_fmt(CW_STAT_TYPE_SIMPLE, 7u, "7");
	expect_fmt(CW_STAT_TYPE_SIMPLE, 999u, "999");
	expect_fmt(CW_STAT_TYPE_SIMPLE, 1000u, "1,000");
	expect_fmt(CW_STAT_TYPE_SIMPLE, 10000u, "10,000");
	expect_fmt(CW_STAT_TYPE_SIMPLE, 1234567u, "1,234,567");
	expect_fmt(CW_STAT_TYPE_SIMPLE, 4294967295u, "4,294,967,295");

	/* TIME (ticks). Seconds hold while minutes <= 0.5, i.e. up to 600
	 * ticks, and Beta appends the raw double there. */
	expect_fmt(CW_STAT_TYPE_TIME, 0u, "0.0 s");
	expect_fmt(CW_STAT_TYPE_TIME, 10u, "0.5 s");
	expect_fmt(CW_STAT_TYPE_TIME, 20u, "1.0 s");
	expect_fmt(CW_STAT_TYPE_TIME, 25u, "1.25 s");
	expect_fmt(CW_STAT_TYPE_TIME, 600u, "30.0 s"); /* minutes == 0.5 exactly */
	expect_fmt(CW_STAT_TYPE_TIME, 601u, "0.50 m");
	expect_fmt(CW_STAT_TYPE_TIME, 1200u, "1.00 m");
	expect_fmt(CW_STAT_TYPE_TIME, 36000u, "30.00 m"); /* hours == 0.5 exactly */
	expect_fmt(CW_STAT_TYPE_TIME, 72000u, "1.00 h");
	expect_fmt(CW_STAT_TYPE_TIME, 864000u, "12.00 h"); /* days == 0.5 exactly */
	expect_fmt(CW_STAT_TYPE_TIME, 1728000u, "1.00 d");
	expect_fmt(CW_STAT_TYPE_TIME, 630720000u, "1.00 y");
	expect_fmt(CW_STAT_TYPE_TIME, 4294967295u, "6.81 y");

	/* DISTANCE (centimetres): the cm -> m and m -> km switches. */
	expect_fmt(CW_STAT_TYPE_DISTANCE, 0u, "0 cm");
	expect_fmt(CW_STAT_TYPE_DISTANCE, 50u, "50 cm"); /* m == 0.5 exactly */
	expect_fmt(CW_STAT_TYPE_DISTANCE, 51u, "0.51 m");
	expect_fmt(CW_STAT_TYPE_DISTANCE, 100u, "1.00 m");
	expect_fmt(CW_STAT_TYPE_DISTANCE, 50000u, "500.00 m"); /* km == 0.5 */
	expect_fmt(CW_STAT_TYPE_DISTANCE, 50001u, "0.50 km");
	expect_fmt(CW_STAT_TYPE_DISTANCE, 100000u, "1.00 km");
	expect_fmt(CW_STAT_TYPE_DISTANCE, 123456u, "1.23 km");
	expect_fmt(CW_STAT_TYPE_DISTANCE, 4294967295u, "42949.67 km");

	/* Total on bad input: never writes past the buffer, never asserts. */
	buf[0] = 'x';
	assert(!stats_format_value(200u, 1u, buf, sizeof(buf)) && buf[0] == '\0');
	assert(!stats_format_value((uint8_t)CW_STAT_TYPE_SIMPLE, 1u, NULL, 8u));
	assert(!stats_format_value((uint8_t)CW_STAT_TYPE_SIMPLE, 1u, buf, 0u));
	assert(!stats_format_value((uint8_t)CW_STAT_TYPE_SIMPLE, 1234567u, tiny,
							   sizeof(tiny))
		   && tiny[0] == '\0');

	puts("PASS the three Beta formatters reproduce Beta's strings");
}

/* Design 7: "a notice belongs to the slot whose actor earned it"; the other
 * view draws nothing. And ack hides THAT toast, not the next one. */
static void test_notice_isolation_and_ack(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	char c[CW_PROFILE_ID_MAX + 1], d[CW_PROFILE_ID_MAX + 1];
	struct ach_notice n;

	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 1u, a, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	assert(achievements_bind(1, 1u, b, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);

	/* Both profiles are fresh, so both carry the openInventory HINT -- a
	 * persistent flag, not a toast, so neither makes peek() true and neither
	 * counts [design 7, R1 LOW 11]. */
	assert(!achievements_notice_peek(0, &n) && n.hint_active && n.ach_id == -1);
	assert(!achievements_notice_peek(1, &n) && n.hint_active);
	{
		struct ach_status st;
		assert(achievements_status(0, &st) && st.notices_queued == 0u);
	}

	/* Slot 1 opens its inventory: openInventory unlocks and toasts, and its
	 * hint goes out. */
	emit_plain(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 1, 1u);
	achievements_tick();
	assert(achievements_notice_peek(1, &n));
	assert(n.ach_id == 0 && !n.hint_active);
	/* The OTHER view draws no toast -- and still has its own hint. */
	assert(!achievements_notice_peek(0, &n));
	assert(n.ach_id == -1 && n.started_ms == 0u && n.hint_active);

	/* Ack clears the taken toast for that slot only. */
	achievements_notice_ack(1);
	assert(!achievements_notice_peek(1, &n) && !n.hint_active);
	/* Acking a slot with nothing on screen is a no-op, not a state change --
	 * above all it does not clear slot 0's hint. */
	achievements_notice_ack(0);
	assert(!achievements_notice_peek(0, &n) && n.hint_active);

	/* Slot 0 now earns its own toast; slot 1 stays acked. */
	emit_plain(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0, 1u);
	achievements_tick();
	assert(achievements_notice_peek(0, &n) && n.ach_id == 0 && !n.hint_active);
	assert(!achievements_notice_peek(1, &n));

	/* A fresh binding on slot 1 brings the hint back (it belongs to the
	 * PROFILE), and no number of ticks turns it into a toast or a count. */
	next_ids(c, d, sizeof(c));
	achievements_unbind(1, 1u);
	assert(achievements_bind(1, 2u, d, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	assert(!achievements_notice_peek(1, &n) && n.hint_active);
	for(int i = 0; i < 30; i++) {
		emit_plain(GE_TICK, GE_AUTH_CLIENT, 1, 2u);
		achievements_tick();
	}
	assert(!achievements_notice_peek(1, &n) && n.hint_active);
	{
		struct ach_status st;
		assert(achievements_status(1, &st));
		assert(st.notices_queued == 0u); /* toasts only, never ticks */
	}
	/* Acking does not take the hint away -- only unlocking does. */
	achievements_notice_ack(1);
	assert(!achievements_notice_peek(1, &n) && n.hint_active);
	emit_plain(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 1, 2u);
	achievements_tick();
	assert(achievements_notice_peek(1, &n) && n.ach_id == 0 && !n.hint_active);
	{
		struct ach_status st;
		assert(achievements_status(1, &st) && st.notices_queued == 1u);
	}
	achievements_notice_ack(1);
	assert(!achievements_notice_peek(1, &n) && !n.hint_active);

	/* Unbinding clears the toast; a slot that does not exist is total. */
	achievements_unbind(1, 2u);
	assert(!achievements_notice_peek(1, &n) && !n.hint_active);
	assert(!achievements_notice_peek(CW_ACH_SLOT_COUNT, &n));
	assert(!achievements_notice_peek(0, NULL));
	achievements_notice_ack(CW_ACH_SLOT_COUNT); /* must not crash */

	achievements_unbind(0, 1u);
	puts("PASS notices are per owner, ack clears only the taken toast, and the "
		 "hint is a flag that outlives it");
}

/* Design 7 / 1.6: the grid row a screen draws -- keys, Beta's signed
 * display coordinates, icon, parent, special, and the three shading
 * states (dark / dimmed / full colour). */
static void test_achievement_rows(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	struct ach_desc d;

	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 1u, a, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);

	assert(achievements_count() == 16); /* AchievementList registers 16 */

	/* openInventory: locked but unlockable (no parent), ordinary frame. */
	assert(achievements_get(0, 0, &d));
	assert(d.id == 0 && d.parent == -1);
	assert(!strcmp(d.name_key, "achievement.openInventory"));
	assert(!strcmp(d.desc_key, "achievement.openInventory.desc"));
	assert(d.column == 0 && d.row == 0 && d.icon_item == 340);
	assert(!d.unlocked && d.can_unlock && !d.special);

	/* mineWood: DARK -- its parent is still locked. */
	assert(achievements_get(0, 1, &d));
	assert(d.parent == 0 && d.column == 2 && d.row == 1 && d.icon_item == 17);
	assert(!d.unlocked && !d.can_unlock);

	/* Unlock the root: it becomes full colour and mineWood becomes dimmed. */
	emit_plain(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0, 1u);
	achievements_tick();
	assert(achievements_get(0, 0, &d) && d.unlocked && d.can_unlock);
	assert(achievements_get(0, 1, &d) && !d.unlocked && d.can_unlock);

	/* Earn mineWood (picking up a log) -- unlocked, and buildWorkBench
	 * becomes the dimmed row. */
	emit_item(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 0, 1u, 17u, 1u);
	achievements_tick();
	assert(achievements_get(0, 1, &d) && d.unlocked);
	assert(achievements_get(0, 2, &d) && !d.unlocked && d.can_unlock);
	assert(d.column == 4 && d.row == -1 && d.parent == 1);

	/* The two setSpecial() rows [design 1.1]. */
	assert(achievements_get(0, 11, &d));
	assert(d.special && d.column == 2 && d.row == 3 && d.parent == 5);
	assert(!strcmp(d.name_key, "achievement.onARail"));
	assert(achievements_get(0, 15, &d));
	assert(d.special && d.column == 8 && d.row == -4 && d.parent == 14);
	assert(d.icon_item == 329 && !strcmp(d.desc_key, "achievement.flyPig.desc"));
	/* Nothing else is special. */
	{
		int i, specials = 0;
		for(i = 0; i < achievements_count(); i++) {
			assert(achievements_get(0, i, &d));
			if(d.special)
				specials++;
		}
		assert(specials == 2);
	}

	/* Both display coordinates are SIGNED and both go negative. */
	assert(achievements_get(0, 7, &d) && d.column == -1 && d.row == -3);
	assert(achievements_get(0, 8, &d) && d.column == 0 && d.row == -5);

	/* And they come from the ONE table every other field comes from, not from
	 * a parallel array in this file (R1 lead item 3): the accessor and the
	 * row agree for all sixteen, and the row order is still Beta's. */
	{
		int i;
		for(i = 0; i < achievements_count(); i++) {
			assert(achievements_get(0, i, &d));
			assert(d.column == ach_column(i) && d.row == ach_row(i));
			assert(d.icon_item == ach_icon_item(i)
				   && d.parent == (int8_t)ach_parent(i));
		}
		assert(ach_column(0) == 0 && ach_row(0) == 0);
		assert(ach_column(15) == 8 && ach_row(15) == -4);
		assert(ach_column(-1) == 0 && ach_row(CW_ACH_COUNT) == 0);
	}

	/* Out-of-range indices are the defined empty row. */
	assert(!achievements_get(0, -1, &d) && d.id == -1 && d.parent == -1);
	assert(!achievements_get(0, achievements_count(), &d) && d.id == -1);
	assert(!achievements_get(0, 0, NULL));

	achievements_unbind(0, 1u);
	puts("PASS achievement rows carry Beta's grid data and the three states");
}

/* Design 7 / 1.6: the three statistics tabs, each cell carrying local,
 * remote and the saturated total. */
static void test_stat_rows(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	struct stat_row row;
	struct stat_cell mined, crafted, used, depleted;
	struct stat_cell grass, dirt;
	int i;

	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));
	/* Slot 0 in a local world, slot 1 joined to a remote server: the two
	 * provenance columns of design 6, in one run. */
	assert(achievements_bind(0, 1u, a, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	assert(achievements_bind(1, 1u, b, ACH_MODE_REMOTE_SERVER) == ACH_BIND_OK);

	emit_plain(GE_JUMPED, GE_AUTH_CLIENT, 0, 1u);
	emit_plain(GE_JUMPED, GE_AUTH_CLIENT, 0, 1u);
	emit_plain(GE_JUMPED, GE_AUTH_CLIENT, 0, 1u);
	emit_item(GE_BLOCK_HARVESTED, GE_AUTH_SERVER, 0, 1u, 1u, 1u); /* stone */
	emit_item(GE_BLOCK_HARVESTED, GE_AUTH_SERVER, 0, 1u, 1u, 1u);
	emit_item(GE_BLOCK_HARVESTED, GE_AUTH_SERVER, 0, 1u, 2u, 1u); /* grass */
	emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 0, 1u, 5u, 4u);	  /* planks */
	/* Client-owned stats DO count on a remote server [design 1.2 point 4]. */
	emit_plain(GE_JUMPED, GE_AUTH_CLIENT, 1, 1u);
	emit_plain(GE_JUMPED, GE_AUTH_CLIENT, 1, 1u);
	achievements_tick();

	/* General tab: ordered, complete, and false exactly past the end. */
	assert(stats_general_iter(0, 0, &row) && row.id == 1000u
		   && !strcmp(row.key, "startGame"));
	assert(!strcmp(row.name_key, "stat.startGame"));
	i = 0;
	while(stats_general_iter(0, i, &row))
		i++;
	assert(i == CW_STAT_COUNT);
	assert(!stats_general_iter(0, CW_STAT_COUNT, &row) && row.id == 0u
		   && row.key == NULL);

	/* The jump row, on the LOCAL side. */
	assert(general_row(0, 2010u, &row) >= 0);
	assert(!strcmp(row.key, "jump") && !strcmp(row.name_key, "stat.jump"));
	assert(row.type == (uint8_t)CW_STAT_TYPE_SIMPLE);
	assert(row.owner == (uint8_t)CW_STAT_OWNER_CLIENT);
	assert(row.value.local == 3u && row.value.remote == 0u
		   && row.value.total == 3u);
	/* The same row for the remote-session owner lands in the other column. */
	assert(general_row(1, 2010u, &row) >= 0);
	assert(row.value.local == 0u && row.value.remote == 2u
		   && row.value.total == 2u);
	/* The type drives the formatter the tab uses. */
	assert(general_row(0, 1100u, &row) >= 0
		   && row.type == (uint8_t)CW_STAT_TYPE_TIME);
	assert(general_row(0, 2000u, &row) >= 0
		   && row.type == (uint8_t)CW_STAT_TYPE_DISTANCE);
	assert(general_row(0, 2022u, &row) >= 0
		   && row.owner == (uint8_t)CW_STAT_OWNER_SERVER);

	/* Blocks tab. */
	assert(stats_block_row(0, 1u, &mined, &crafted, &used));
	assert(mined.local == 2u && mined.remote == 0u && mined.total == 2u);
	assert(crafted.total == 0u && used.total == 0u);
	/* Canonical fold: grass reports dirt's counter [design 2]. */
	assert(stats_block_row(0, 2u, &grass, NULL, NULL));
	assert(stats_block_row(0, 3u, &dirt, NULL, NULL));
	assert(grass.total == 1u && dirt.total == 1u);
	/* A disableStats() block has no mine counter at all. */
	assert(stats_block_row(0, 7u, &mined, NULL, NULL) && mined.total == 0u);
	assert(!stats_block_row(0, CW_STAT_BLOCK_COUNT, &mined, NULL, NULL)
		   && mined.total == 0u);

	/* Items tab: Used / Depleted / Crafted, in that order. */
	assert(stats_item_row(0, 5u, &used, &depleted, &crafted));
	assert(crafted.local == 4u && crafted.total == 4u);
	assert(used.total == 0u && depleted.total == 0u);
	assert(!stats_item_row(0, (uint16_t)ITEMS_MAX, &used, NULL, NULL)
		   && used.total == 0u);

	achievements_unbind(0, 1u);
	achievements_unbind(1, 1u);
	puts("PASS statistic rows carry local, remote and the saturated total");
}

/* "Every function returns a defined empty/false result for an unbound slot
 * -- never assert, never read another slot." */
static void test_unbound_and_bad_slot_are_empty(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	struct stat_row row;
	struct stat_cell cell;
	struct ach_desc d;
	struct ach_notice n;
	struct ach_status st;

	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));
	/* Only slot 1 is bound, and it HAS progress: if any query fell back to
	 * "the other slot", these assertions would see slot 1's counters. */
	assert(achievements_bind(1, 5u, b, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	emit_plain(GE_JUMPED, GE_AUTH_CLIENT, 1, 5u);
	emit_plain(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 1, 5u);
	achievements_tick();
	assert(general_row(1, 2010u, &row) >= 0 && row.value.total == 1u);

	/* Slot 0: valid, but nothing bound. */
	assert(!stats_general_iter(0, 0, &row) && row.id == 0u
		   && row.key == NULL && row.value.total == 0u);
	assert(!stats_block_row(0, 1u, &cell, NULL, NULL) && cell.total == 0u);
	assert(!stats_item_row(0, 5u, &cell, NULL, NULL) && cell.total == 0u);
	assert(!achievements_get(0, 0, &d) && d.id == -1 && d.parent == -1
		   && !d.unlocked && !d.can_unlock);
	assert(!achievements_notice_peek(0, &n) && n.ach_id == -1);
	/* status is the one call that stays meaningful: it REPORTS unbound. */
	assert(achievements_status(0, &st) && !st.bound);
	assert(st.accepted == 0u && st.notices_queued == 0u);

	/* A slot that does not exist. */
	assert(!stats_general_iter(CW_ACH_SLOT_COUNT, 0, &row));
	assert(!stats_block_row(CW_ACH_SLOT_COUNT, 1u, &cell, NULL, NULL));
	assert(!stats_item_row(CW_ACH_SLOT_COUNT, 5u, &cell, NULL, NULL));
	assert(!achievements_get(CW_ACH_SLOT_COUNT, 0, &d));
	assert(!achievements_notice_peek(CW_ACH_SLOT_COUNT, &n));
	assert(!achievements_status(CW_ACH_SLOT_COUNT, &st) && !st.bound);
	assert(!achievements_status(0, NULL));

	/* And after an unbind the bound slot's rows go empty too, while its
	 * counters are still resident in the module. */
	achievements_unbind(1, 5u);
	assert(!stats_general_iter(1, 0, &row));
	assert(!achievements_get(1, 0, &d));
	assert(achievements_status(1, &st) && !st.bound);

	puts("PASS unbound and non-existent slots yield defined empty rows");
}

/* Design 7: the status API carries load state, save failures, the last
 * error, the per-reason rejection counters and progress_incomplete. */
static void test_status_reports_failure_and_rejection(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	struct ach_status st;

	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 7u, a, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);

	assert(achievements_status(0, &st));
	assert(st.bound && st.generation == 7u
		   && st.mode == (uint8_t)ACH_MODE_LOCAL_WORLD);
	assert(!strcmp(st.profile_id, a));
	assert(st.load == (uint8_t)STATS_LOAD_MISSING); /* a fresh profile */
	assert(st.save_failures == 0u && st.last_save_error == 0u);
	assert(!st.dirty && !st.progress_incomplete);
	assert(st.rejected[ACH_REJECT_STALE_GENERATION] == 0u);

	/* A rejection: a previous player's event for this slot. */
	emit_plain(GE_JUMPED, GE_AUTH_CLIENT, 0, 99u);
	achievements_tick();
	assert(achievements_status(0, &st));
	assert(st.rejected[ACH_REJECT_STALE_GENERATION] == 1u);
	assert(st.accepted == 0u && !st.dirty);

	/* Real progress, then a save that fails at the write stage. */
	emit_plain(GE_JUMPED, GE_AUTH_CLIENT, 0, 7u);
	achievements_tick();
	assert(achievements_status(0, &st) && st.accepted == 1u && st.dirty);

	stats_io_fail_at(STATS_IO_STAGE_WRITE);
	achievements_flush_all();
	assert(achievements_status(0, &st));
	assert(st.save_failures == 1u);
	assert(st.last_save_error == (uint8_t)STATS_SAVE_ERR_WRITE);
	assert(st.dirty); /* a failed write never clears the dirty flag */
	assert(st.rejected[ACH_REJECT_STALE_GENERATION] == 1u);

	/* The injection is one-shot: the next flush succeeds and the failure
	 * count stays on record, while the resolved error clears. */
	achievements_flush_all();
	assert(achievements_status(0, &st));
	assert(!st.dirty && st.save_failures == 1u);
	assert(st.last_save_error == (uint8_t)STATS_SAVE_OK);

	achievements_unbind(0, 7u);
	puts("PASS status reports the injected save failure and the rejection");
}

int main(void) {
	test_fixture_dir("test_achievements_api_tmp", s_dir, sizeof(s_dir));
	make_dir(s_dir);

	test_format_values();
	test_notice_isolation_and_ack();
	test_achievement_rows();
	test_stat_rows();
	test_unbound_and_bad_slot_are_empty();
	test_status_reports_failure_and_rejection();

	achievements_shutdown(); /* release the module block before the leak check */
	test_fixture_dir_cleanup(s_dir);
	puts("PASS test_achievements_api");
	return 0;
}
