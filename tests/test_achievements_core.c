/* Copyright (c) 2026 CaveWii_v2 contributors */
/* docs/achievements_design.md section 9, row `test_achievements_core`:
 *
 *   each of the 16 triggers fires from its event; each non-trigger (wrong
 *   item id, ghast/slime/wolf kill, pig fall <= 5, rail sq-dist 999) does
 *   not; parent locked -> dropped and not counted; repeat increments count
 *   without a second toast; toast content and 3.0 s window
 *
 * Plus the plain counting the same drain owes design 1.4 for the kinds that
 * are not achievement triggers (the LCE-02 formatting and aliasing goldens
 * are brief B3, not this file).
 *
 * Every case runs against a FRESH profile id inside one per-process fixture
 * directory, so no case can inherit a file another case wrote.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "support/test_fixture_dir.h"

#include "entity/monsters_object.h"
#include "game/achievements/achievements.h"
/* For the one assertion that B4 READS a counter through the same fold B2
 * writes it with (R1 lead item 2); everything else here is B2's own surface. */
#include "game/achievements/achievements_api.h"

static char s_dir[128];
static uint64_t s_gen = 7u;
static int s_case;

static void make_dir(const char* dir) {
#ifdef _WIN32
	(void)mkdir(dir);
#else
	(void)mkdir(dir, 0777);
#endif
}

/* ---------------------------------------------------------------- helpers */

static struct game_event mk(uint8_t kind, uint8_t authority) {
	struct game_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.kind = kind;
	ev.authority = authority;
	ev.who.slot = 0;
	ev.who.generation = s_gen;
	return ev;
}

static void emit_item(uint8_t kind, uint8_t authority, uint16_t id,
					  uint16_t count) {
	struct game_event ev = mk(kind, authority);
	ev.u.item.id = id;
	ev.u.item.count = count;
	game_event_emit(&ev);
}

static void emit_move(uint8_t kind, uint8_t authority, uint8_t sub,
					  int32_t cm) {
	struct game_event ev = mk(kind, authority);
	ev.sub = sub;
	ev.u.move.cm = cm;
	game_event_emit(&ev);
}

/* GE_PIG_FELL_WITH_RIDER carries the fall distance in BLOCKS as a float,
 * because EntityPig.fall tests `f > 5F` on the float (R2 MED 5). */
static void emit_pig_fall(float f) {
	struct game_event ev = mk(GE_PIG_FELL_WITH_RIDER, GE_AUTH_SERVER);
	ev.u.fall.f = f;
	game_event_emit(&ev);
}

static void emit_ride(uint8_t sub, int32_t x, int32_t y, int32_t z,
					  int32_t cm) {
	struct game_event ev = mk(GE_RIDE, GE_AUTH_CLIENT);
	ev.sub = sub;
	ev.u.ride.x = x;
	ev.u.ride.y = y;
	ev.u.ride.z = z;
	ev.u.ride.cm = cm;
	game_event_emit(&ev);
}

static void emit_simple(uint8_t kind, uint8_t authority, uint8_t sub) {
	struct game_event ev = mk(kind, authority);
	ev.sub = sub;
	game_event_emit(&ev);
}

static void emit_n(uint8_t kind, uint8_t authority, int32_t amount) {
	struct game_event ev = mk(kind, authority);
	ev.u.n.amount = amount;
	game_event_emit(&ev);
}

static const struct stats_profile* p0(void) {
	return achievements_debug_profile(0);
}

static struct ach_status status0(void) {
	struct ach_status st;
	memset(&st, 0, sizeof(st));
	assert(achievements_debug_status(0, &st));
	return st;
}

/* The whole notice struct, whatever it holds: `hint_active` is a flag on it,
 * not a toast, so it is read even when there is no toast at all. */
static struct ach_notice notice0(void) {
	struct ach_notice n;
	memset(&n, 0, sizeof(n));
	(void)achievements_debug_notice(0, &n);
	return n;
}

static uint32_t total_of(const struct stats_pair* p) {
	return stats_counter_total(p);
}

static uint32_t general_of(uint32_t stat_id) {
	int i = stat_general_index(stat_id);
	assert(i >= 0);
	return stats_counter_total(&p0()->general[i]);
}

/* A fresh module + a profile id no earlier case used. */
static void fresh_local(void) {
	char id[CW_PROFILE_ID_MAX + 1];
	snprintf(id, sizeof(id), "core%d", s_case++);
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, s_gen, id, ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
}

/* The exact event(s) design 1.1 says fire achievement `a`. */
static void emit_trigger(int a) {
	switch(a) {
		case 0: emit_simple(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0); break;
		case 1: emit_item(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 17, 1); break;
		case 2: emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 58, 1); break;
		case 3: emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 270, 1); break;
		case 4: emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 61, 1); break;
		case 5: emit_item(GE_FURNACE_TAKEN, GE_AUTH_SERVER, 265, 1); break;
		case 6: emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 290, 1); break;
		case 7: emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 297, 1); break;
		case 8: emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 354, 1); break;
		case 9: emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 274, 1); break;
		case 10: emit_item(GE_FURNACE_TAKEN, GE_AUTH_SERVER, 350, 1); break;
		case 11:
			/* The origin is set on the first mounted move and NOT checked on
			 * it (Beta's `else if`); 30^2 + 10^2 = 1000 is the exact
			 * threshold [design 1.1 row 11]. */
			emit_ride(GE_MOUNT_MINECART, 0, 0, 0, 10);
			emit_ride(GE_MOUNT_MINECART, 30, 10, 0, 10);
			break;
		case 12: emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 268, 1); break;
		case 13: emit_simple(GE_KILLED, GE_AUTH_SERVER, MONSTER_ZOMBIE); break;
		case 14: emit_item(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 334, 1); break;
		case 15: emit_pig_fall(5.01F); break;
		default: assert(0);
	}
}

static void unlock_ancestors(int a) {
	int chain[CW_ACH_COUNT];
	int n = 0;
	for(int p = ach_parent(a); p >= 0; p = ach_parent(p))
		chain[n++] = p;
	for(int i = n - 1; i >= 0; i--) {
		emit_trigger(chain[i]);
		achievements_tick();
		assert(ach_unlocked(p0(), chain[i]));
	}
}

/* ------------------------------------------------------------------ cases */

static void test_all_sixteen_triggers(void) {
	for(int a = 0; a < CW_ACH_COUNT; a++) {
		fresh_local();
		unlock_ancestors(a);
		assert(!ach_unlocked(p0(), a));
		emit_trigger(a);
		achievements_tick();
		assert(ach_unlocked(p0(), a));
		/* Beta counts the unlock itself: readStat with amount 1, ONCE for the
		 * one triggering event -- `>= 1` would have passed for any number. */
		assert(stats_counter_total(&p0()->ach[a]) == 1u);
		assert(p0()->ach_first[a] == (uint8_t)STATS_FIRST_LOCAL);
	}
	puts("PASS all sixteen Beta achievements fire from their own event");
}

static void test_non_triggers(void) {
	fresh_local();
	/* Unlock the whole spine so nothing below is merely parent-gated: the
	 * point of this case is that the WRONG payload does not fire. */
	for(int a = 0; a < CW_ACH_COUNT; a++)
		if(a != 11 && a != 13 && a != 15) {
			unlock_ancestors(a);
			emit_trigger(a);
			achievements_tick();
		}
	assert(ach_unlocked(p0(), 12) && ach_unlocked(p0(), 14));
	assert(!ach_unlocked(p0(), 11) && !ach_unlocked(p0(), 13)
		   && !ach_unlocked(p0(), 15));
	struct ach_status before = status0();

	/* Wrong item ids: leaves are not wood, planks are not a workbench, raw
	 * iron ore is not an ingot, raw fish is not cooked fish. */
	emit_item(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 18, 1);
	emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 5, 1);
	emit_item(GE_FURNACE_TAKEN, GE_AUTH_SERVER, 15, 1);
	emit_item(GE_FURNACE_TAKEN, GE_AUTH_SERVER, 349, 1);
	/* NOT an EntityMob: ghast is EntityFlying, slime is EntityLiving, wolf
	 * is an animal [design 1.1 row 13]. */
	emit_simple(GE_KILLED, GE_AUTH_SERVER, MONSTER_GHAST);
	emit_simple(GE_KILLED, GE_AUTH_SERVER, MONSTER_SLIME_BIG);
	emit_simple(GE_KILLED, GE_AUTH_SERVER, MONSTER_WOLF);
	emit_simple(GE_KILLED, GE_AUTH_SERVER, MONSTER_COW);
	/* EntityPig.fall needs f > 5.0F, so exactly 5.0 blocks is a non-trigger. */
	emit_pig_fall(5.0F);
	/* Rail: strictly below the squared 1000. The design row says "999", but
	 * 999 = 8*124 + 7 is one of the integers Legendre's three-square theorem
	 * excludes, so no integer block offset can produce it; 998 =
	 * 31^2 + 6^2 + 1^2 is the closest reachable value below the threshold. */
	emit_ride(GE_MOUNT_MINECART, 0, 0, 0, 10);
	emit_ride(GE_MOUNT_MINECART, 31, 6, 1, 10);
	achievements_tick();

	assert(!ach_unlocked(p0(), 11));
	assert(!ach_unlocked(p0(), 13));
	assert(!ach_unlocked(p0(), 15));
	assert(stats_counter_total(&p0()->craft[5]) == 1u); /* still a craft */
	/* None of these was a rejection: they were accepted and simply did not
	 * match an achievement condition. */
	struct ach_status after = status0();
	for(int r = 0; r < ACH_REJECT_COUNT; r++)
		assert(after.rejected[r] == before.rejected[r]);
	/* mobKills counts every kill, achievement or not (Beta:
	 * addToPlayerScore is not the same call as onKillEntity). */
	assert(general_of(2023u) == 4u); /* the four kills just emitted */
	puts("PASS non-triggers: wrong ids, non-EntityMob kills, 5.0 pig fall, "
		 "sub-1000 rail");
}

static void test_parent_locked_drops(void) {
	fresh_local();
	/* openInventory is still locked, so mineWood is dropped ENTIRELY: not
	 * counted, not deferred [design 1.2 point 1]. */
	emit_trigger(1);
	emit_trigger(1);
	achievements_tick();
	assert(!ach_unlocked(p0(), 1));
	assert(stats_counter_total(&p0()->ach[1]) == 0u);
	struct ach_status st = status0();
	/* A parent-locked drop is the RULE working, so it is its own status
	 * counter and not a rejected[] reason (R2 LOW 11): the same event can
	 * apply a counter and be `accepted` at the same time, and counting it in
	 * both places made one event two entries in the ledger. */
	assert(st.ach_dropped_parent_locked == 2u);
	for(int r = 0; r < ACH_REJECT_COUNT; r++)
		assert(st.rejected[r] == 0u);
	assert(st.notices_queued == 0u);
	/* This particular event applies NOTHING but the award (Beta has no
	 * items-picked-up counter), so nothing was accepted either. */
	assert(st.accepted == 0u);
	/* The notice slot is EMPTY, and saying so has to be able to fail: the
	 * out-parameter is poisoned first, so a debug_notice() that returned false
	 * without writing anything would be caught instead of reading back the
	 * -1 it never stored (R1 LOW 16). */
	struct ach_notice n;
	memset(&n, 0x5A, sizeof(n));
	assert(!achievements_debug_notice(0, &n));
	assert(n.ach_id == -1 && n.started_ms == 0u);
	assert(status0().notices_queued == 0u);

	/* And it is NOT deferred: unlocking the parent afterwards does not
	 * resurrect the two dropped events. */
	emit_trigger(0);
	achievements_tick();
	assert(ach_unlocked(p0(), 0));
	assert(stats_counter_total(&p0()->ach[1]) == 0u);
	puts("PASS parent locked -> dropped entirely, counted by reason, never "
		 "deferred");
}

static void test_repeat_counts_without_second_toast(void) {
	fresh_local();
	unlock_ancestors(1);
	uint32_t toasts_before = status0().notices_queued;

	emit_trigger(1);
	achievements_tick();
	assert(stats_counter_total(&p0()->ach[1]) == 1u);
	uint32_t toasts_after_first = status0().notices_queued;
	assert(toasts_after_first == toasts_before + 1u);

	for(int i = 0; i < 4; i++) {
		emit_trigger(1);
		achievements_tick();
	}
	/* Beta's readStat increments on EVERY repeat; queueTakenAchievement only
	 * runs while the achievement was still locked [design 1.2 point 1]. */
	assert(stats_counter_total(&p0()->ach[1]) == 5u);
	assert(status0().notices_queued == toasts_after_first);
	puts("PASS repeats increment the count and queue exactly one toast");
}

static void test_toast_content_and_window(void) {
	/* (a) an unlock toast. */
	fresh_local();
	unlock_ancestors(2);
	emit_trigger(2);
	achievements_tick();
	struct ach_notice n;
	assert(achievements_debug_notice(0, &n));
	assert(n.ach_id == 2);
	uint32_t now = achievements_debug_now_ms();
	assert(now >= n.started_ms);
	uint32_t elapsed = now - n.started_ms;
	/* d = (now - started) / 3000 is inside 0..1, i.e. the window is open and
	 * has just begun [design 1.3]. */
	assert(elapsed < ACH_NOTICE_WINDOW_MS);
	assert(elapsed < 500u);

	/* (b) the openInventory hint is a persistent FLAG, not a queued toast
	 * (R1 LOW 11): it is on from the moment a profile with openInventory
	 * locked is bound, it never bumps notices_queued however many ticks pass,
	 * and it never touches the toast's start stamp. */
	fresh_local();
	assert(!ach_unlocked(p0(), 0));
	assert(notice0().hint_active);	  /* before any tick at all */
	assert(notice0().ach_id == -1);	  /* and it is NOT a toast */
	assert(status0().notices_queued == 0u);
	for(int i = 0; i < 40; i++) {
		emit_simple(GE_TICK, GE_AUTH_CLIENT, 0);
		achievements_tick();
	}
	assert(notice0().hint_active && notice0().ach_id == -1);
	assert(status0().notices_queued == 0u); /* 40 ticks, still zero toasts */

	/* Unlocking it turns the hint off and queues exactly ONE toast, whose
	 * window starts now rather than being re-stamped by the next tick. */
	emit_trigger(0);
	achievements_tick();
	assert(!notice0().hint_active);
	assert(status0().notices_queued == 1u);
	n = notice0();
	assert(n.ach_id == 0);
	uint32_t started = n.started_ms;
	emit_simple(GE_TICK, GE_AUTH_CLIENT, 0);
	achievements_tick();
	assert(status0().notices_queued == 1u);
	assert(notice0().started_ms == started && !notice0().hint_active);

	/* A profile RELOADED with openInventory already unlocked never shows the
	 * hint, and one without it does -- the flag follows the profile. */
	assert(achievements_bind(1, s_gen, "hintcheck", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	struct ach_notice n1;
	memset(&n1, 0, sizeof(n1));
	(void)achievements_debug_notice(1, &n1);
	assert(n1.hint_active && n1.ach_id == -1);
	puts("PASS toast content, the 3.0 s window and the persistent info hint");
}

/* R1 LOW 11 / lead item 1: ack clears the taken toast and nothing else. */
static void test_notice_clear_leaves_the_hint(void) {
	fresh_local();
	assert(notice0().hint_active);
	unlock_ancestors(1); /* unlocks openInventory, so the hint goes off */
	assert(!notice0().hint_active);
	emit_trigger(1);
	achievements_tick();
	assert(notice0().ach_id == 1); /* mineWood's toast is the current one */
	uint32_t queued = status0().notices_queued;

	achievements_notice_clear(0);
	assert(notice0().ach_id == -1);
	assert(status0().notices_queued == queued); /* clearing queues nothing */
	achievements_notice_clear(0);			   /* a second clear is a no-op */
	assert(notice0().ach_id == -1);
	achievements_notice_clear(CW_ACH_SLOT_COUNT); /* must not crash */

	/* On a profile whose openInventory is still locked, clearing the toast
	 * leaves the hint exactly where it was. */
	fresh_local();
	assert(notice0().hint_active && notice0().ach_id == -1);
	achievements_notice_clear(0);
	assert(notice0().hint_active);
	emit_simple(GE_TICK, GE_AUTH_CLIENT, 0);
	achievements_tick();
	assert(notice0().hint_active && status0().notices_queued == 0u);
	puts("PASS clearing a toast never touches the hint, and counts nothing");
}

/* R1 LOW 9, now under design 5.2 v4: flyPig is awarded once per fall that
 * actually cleared Beta's `f > 5.0F`, and a full main ring cannot change that
 * because GE_PIG_FELL_WITH_RIDER is an ORDERED kind -- it goes to the ordered
 * overflow ring with its own distance instead of folding into a MAX plus a
 * reconstructed `qualifying` count. Same outcomes, exact by construction. */
static void test_pig_fold_awards_per_qualifying_fall(void) {
	static const struct {
		float f[3];
		int n;
		uint32_t want;
	} cases[] = {
		{{6.0F, 1.0F, 1.0F}, 3, 1u}, /* one qualifying fall among two shorts */
		{{6.0F, 7.0F, 0.0F}, 2, 2u}, /* two qualifying falls */
		{{5.0F, 1.0F, 0.0F}, 2, 0u}, /* exactly 5.0 blocks never fires */
		/* THE R2 MED 5 CASE. 5.004 blocks is `> 5F` in Beta and rounds to
		 * exactly 500 centimetres, which the old `amount > 500` predicate
		 * rejected: every value in (5.0, 5.005] was a silent miss. */
		{{5.004F, 0.0F, 0.0F}, 1, 1u},
	};
	for(size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		fresh_local();
		unlock_ancestors(15);
		/* Fill the main ring with a FOLDABLE kind so everything after it takes
		 * the stalled-drain path -- which for a pig fall is now the ordered
		 * overflow ring, not the coalescer. */
		for(int i = 0; i < CW_ACH_RING_CAPACITY; i++)
			emit_simple(GE_JUMPED, GE_AUTH_CLIENT, 0);
		uint32_t folds = status0().ring_folds;
		for(int i = 0; i < cases[c].n; i++)
			emit_pig_fall(cases[c].f[i]);
		/* Not one of them folded: each is queued whole, in order. */
		assert(status0().ring_folds == folds);
		assert(status0().overflow_used == (uint32_t)cases[c].n);
		assert(status0().overflow_peak == (uint32_t)cases[c].n);
		achievements_tick();
		assert(status0().overflow_used == 0u);
		assert(total_of(&p0()->ach[15]) == cases[c].want);
		assert(ach_unlocked(p0(), 15) == (cases[c].want > 0u));
		assert(!status0().progress_incomplete);
	}
	puts("PASS a pig fall past a full ring awards flyPig once per QUALIFYING "
		 "fall, through the ordered overflow ring");
}

/* R1 LOW 15: `accepted` means "this much progress landed", not "the matrix
 * let it through". */
static void test_accepted_counts_only_what_was_applied(void) {
	fresh_local();
	struct ach_status before = status0();

	/* Beta gives leaves no mineBlock counter at all (disableStats), so this
	 * event applies nothing -- and it is not a REJECTION either. */
	emit_item(GE_BLOCK_HARVESTED, GE_AUTH_SERVER, 18, 1);
	/* A pickup of something no achievement watches: Beta has no "items picked
	 * up" statistic, so there is nothing to apply. */
	emit_item(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 1, 1);
	/* An item id past the registry has no counter. */
	emit_item(GE_ITEM_USED, GE_AUTH_SERVER, (uint16_t)ITEMS_MAX, 1);
	/* The three F3 registration gates are the same shape of non-event: Beta
	 * registers no counter, so nothing lands and nothing is `accepted`. */
	emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 3, 1);   /* dirt: uncraftable */
	emit_item(GE_ITEM_BROKEN, GE_AUTH_SERVER, 61, 1);  /* no block is damagable */
	emit_item(GE_BLOCK_HARVESTED, GE_AUTH_SERVER, 97, 1);  /* CaveWii-only id */
	emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 97, 1);
	emit_item(GE_ITEM_USED, GE_AUTH_SERVER, 97, 1);
	emit_item(GE_ITEM_BROKEN, GE_AUTH_SERVER, 97, 1);
	emit_item(GE_BLOCK_HARVESTED, GE_AUTH_SERVER, 200, 1); /* iron chest */
	emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 200, 1);
	emit_item(GE_ITEM_USED, GE_AUTH_SERVER, 200, 1);
	emit_item(GE_ITEM_BROKEN, GE_AUTH_SERVER, 200, 1);
	achievements_tick();
	struct ach_status after = status0();
	assert(after.accepted == before.accepted);
	for(int r = 0; r < ACH_REJECT_COUNT; r++)
		assert(after.rejected[r] == before.rejected[r]);
	assert(total_of(&p0()->mine[18]) == 0u);
	/* Not one counter moved -- and in particular a net-new block did not
	 * quietly borrow a Beta id's counter. */
	for(int b = 0; b < CW_STAT_BLOCK_COUNT; b++)
		assert(total_of(&p0()->mine[b]) == 0u
			   && total_of(&p0()->craft[b]) == 0u
			   && total_of(&p0()->use[b]) == 0u
			   && total_of(&p0()->brk[b]) == 0u);

	/* The same kind on a block that DOES have a counter is accepted. */
	emit_item(GE_BLOCK_HARVESTED, GE_AUTH_SERVER, 3, 1);
	achievements_tick();
	assert(status0().accepted == before.accepted + 1u);
	assert(total_of(&p0()->mine[3]) == 1u);
	puts("PASS accepted counts events that were APPLIED, not merely allowed");
}

/* R1 lead item 2: Beta's replaceAllSimilarBlocks aliases craftItem / useItem /
 * breakItem exactly as it aliases mineBlock -- but each array folds through
 * its OWN registration set, so the fold and the gate are asserted together
 * (F3). */
static void test_item_counters_use_the_block_alias(void) {
	fresh_local();
	/* useItem registers every block id Beta has, so the fold is visible
	 * there: grass and farmland both land on dirt's use counter. */
	emit_item(GE_ITEM_USED, GE_AUTH_SERVER, 2, 1);
	emit_item(GE_ITEM_USED, GE_AUTH_SERVER, 60, 1);
	/* craftItem registers only recipe/smelting outputs. The lit furnace is
	 * not one, the idle furnace is (RecipesCrafting.java:23), and the pair
	 * folds -- so this is the craft fold under the gate. */
	emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 62, 1);
	emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 61, 2);
	/* A disableStats() block has no mine counter and still has a USE counter
	 * of its own: that is why the item arrays fold through
	 * stat_alias_block_in() and not through stat_canonical_block(). */
	emit_item(GE_ITEM_USED, GE_AUTH_SERVER, 64, 1); /* wooden door */
	achievements_tick();

	assert(total_of(&p0()->use[3]) == 2u); /* 1 grass + 1 farmland */
	assert(total_of(&p0()->use[2]) == 0u && total_of(&p0()->use[60]) == 0u);
	assert(total_of(&p0()->craft[61]) == 3u); /* 1 lit + 2 idle furnace */
	assert(total_of(&p0()->craft[62]) == 0u);
	assert(total_of(&p0()->use[64]) == 1u);
	assert(stat_canonical_block(64) == CW_STAT_BLOCK_NONE);
	assert(stat_alias_block_in(CW_STAT_ARRAY_USE, 64) == 64);

	/* The GATES, on the same drain: Beta registers no craftItem counter for a
	 * block that is not a recipe output (dirt, grass, farmland, the door) and
	 * no breakItem counter for ANY block id -- `Item.isDamagable()` is
	 * `maxDamage > 0` and no block item has one -- so these reach nothing. */
	emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 2, 1);
	emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 60, 2);
	emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 64, 1);
	emit_item(GE_ITEM_BROKEN, GE_AUTH_SERVER, 2, 1);
	emit_item(GE_ITEM_BROKEN, GE_AUTH_SERVER, 3, 1);
	emit_item(GE_ITEM_BROKEN, GE_AUTH_SERVER, 61, 1);
	achievements_tick();
	assert(total_of(&p0()->craft[2]) == 0u && total_of(&p0()->craft[3]) == 0u);
	assert(total_of(&p0()->craft[60]) == 0u && total_of(&p0()->craft[64]) == 0u);
	assert(total_of(&p0()->craft[61]) == 3u); /* untouched by the dropped ones */
	for(int b = 0; b < CW_STAT_BLOCK_COUNT; b++)
		assert(total_of(&p0()->brk[b]) == 0u);

	/* B4 reads them back through the same fold, so the screens and the
	 * counters always name one bucket -- and a gated cell reads as zero on
	 * both sides of a pair, never as somebody else's count. */
	struct stat_cell mined, crafted, used;
	assert(stats_block_row(0, 2u, &mined, &crafted, &used));
	assert(used.total == 2u && crafted.total == 0u);
	assert(stats_block_row(0, 3u, &mined, &crafted, &used));
	assert(used.total == 2u && crafted.total == 0u);
	assert(stats_block_row(0, 62u, &mined, &crafted, &used));
	assert(crafted.total == 3u);
	assert(stats_block_row(0, 61u, &mined, &crafted, &used));
	assert(crafted.total == 3u);
	puts("PASS craft/use/break counters fold onto the aliased block id, under "
		 "each array's own registration gate");
}

static void test_plain_counters(void) {
	fresh_local();
	emit_simple(GE_TICK, GE_AUTH_CLIENT, 0);
	emit_simple(GE_TICK, GE_AUTH_CLIENT, 0);
	emit_move(GE_MOVED, GE_AUTH_CLIENT, GE_MOVE_WALK, 120);
	emit_move(GE_MOVED, GE_AUTH_CLIENT, GE_MOVE_SWIM, 7);
	emit_move(GE_MOVED, GE_AUTH_CLIENT, GE_MOVE_DIVE, 9);
	emit_move(GE_MOVED, GE_AUTH_CLIENT, GE_MOVE_CLIMB, 11);
	emit_move(GE_MOVED, GE_AUTH_CLIENT, GE_MOVE_FLY, 30);
	emit_move(GE_FELL, GE_AUTH_CLIENT, 0, 250);
	emit_simple(GE_JUMPED, GE_AUTH_CLIENT, 0);
	emit_simple(GE_DROPPED, GE_AUTH_CLIENT, 0);
	emit_simple(GE_GAME_STARTED, GE_AUTH_CLIENT, GE_WORLD_NEW);
	emit_simple(GE_GAME_STARTED, GE_AUTH_CLIENT, GE_WORLD_LOADED);
	emit_simple(GE_GAME_LEFT, GE_AUTH_CLIENT, 0);
	emit_n(GE_DAMAGE_DEALT, GE_AUTH_SERVER, 12);
	emit_n(GE_DAMAGE_TAKEN, GE_AUTH_SERVER, 5);
	emit_simple(GE_DIED, GE_AUTH_SERVER, 0);
	emit_simple(GE_FISH_CAUGHT, GE_AUTH_SERVER, 0);
	emit_simple(GE_KILLED_PLAYER, GE_AUTH_SERVER, 0);
	emit_item(GE_ITEM_USED, GE_AUTH_SERVER, 270, 1);
	emit_item(GE_ITEM_BROKEN, GE_AUTH_SERVER, 270, 1);
	/* mineBlock is folded onto the canonical id at count time: grass (2) and
	 * farmland (60) both land on dirt (3) [design 2]. */
	emit_item(GE_BLOCK_HARVESTED, GE_AUTH_SERVER, 2, 1);
	emit_item(GE_BLOCK_HARVESTED, GE_AUTH_SERVER, 60, 1);
	emit_item(GE_BLOCK_HARVESTED, GE_AUTH_SERVER, 3, 1);
	/* Beta registers leaves with disableStats(): no counter at all. */
	emit_item(GE_BLOCK_HARVESTED, GE_AUTH_SERVER, 18, 1);
	/* Riding counts distance even when the rail rule does not fire. */
	emit_ride(GE_MOUNT_BOAT, 0, 0, 0, 40);
	emit_ride(GE_MOUNT_PIG, 0, 0, 0, 20);
	achievements_tick();

	assert(general_of(1100u) == 2u);	/* playOneMinute  */
	assert(general_of(2000u) == 120u);	/* walkOneCm      */
	assert(general_of(2001u) == 7u);	/* swimOneCm      */
	assert(general_of(2005u) == 9u);	/* diveOneCm      */
	assert(general_of(2003u) == 11u);	/* climbOneCm     */
	assert(general_of(2004u) == 30u);	/* flyOneCm       */
	assert(general_of(2002u) == 250u);	/* fallOneCm      */
	assert(general_of(2010u) == 1u);	/* jump           */
	assert(general_of(2011u) == 1u);	/* drop           */
	assert(general_of(1000u) == 2u);	/* startGame      */
	assert(general_of(1001u) == 1u);	/* createWorld    */
	assert(general_of(1002u) == 1u);	/* loadWorld      */
	assert(general_of(1004u) == 1u);	/* leaveGame      */
	assert(general_of(2020u) == 12u);	/* damageDealt    */
	assert(general_of(2021u) == 5u);	/* damageTaken    */
	assert(general_of(2022u) == 1u);	/* deaths         */
	assert(general_of(2025u) == 1u);	/* fishCaught     */
	assert(general_of(2024u) == 1u);	/* playerKills    */
	assert(general_of(2007u) == 40u);	/* boatOneCm      */
	assert(general_of(2008u) == 20u);	/* pigOneCm       */
	assert(stats_counter_total(&p0()->use[270]) == 1u);
	assert(stats_counter_total(&p0()->brk[270]) == 1u);
	assert(stats_counter_total(&p0()->mine[3]) == 3u);
	assert(stats_counter_total(&p0()->mine[2]) == 0u);
	assert(stats_counter_total(&p0()->mine[60]) == 0u);
	assert(stats_counter_total(&p0()->mine[18]) == 0u);
	puts("PASS plain statistic counting for every non-achievement kind");
}

static void test_malformed_events_are_counted(void) {
	fresh_local();
	struct game_event ev = mk(GE_KIND_COUNT, GE_AUTH_CLIENT);
	game_event_emit(&ev);
	ev = mk(GE_MOVED, GE_AUTH_CLIENT);
	ev.sub = GE_MOVE_KIND_COUNT;
	game_event_emit(&ev);
	ev = mk(GE_MOVED, GE_AUTH_CLIENT);
	ev.u.move.cm = -1;
	game_event_emit(&ev);
	ev = mk(GE_REMOTE_STAT, GE_AUTH_CLIENT); /* must be PACKET200 */
	game_event_emit(&ev);
	achievements_tick();
	assert(status0().rejected[ACH_REJECT_BAD_EVENT] == 4u);
	assert(status0().accepted == 0u);

	/* An unaddressable slot cannot be counted per slot, so it has its own
	 * counter rather than being attributed to somebody. */
	uint32_t bad = achievements_debug_bad_slot_drops();
	ev = mk(GE_JUMPED, GE_AUTH_CLIENT);
	ev.who.slot = CW_ACH_SLOT_COUNT;
	game_event_emit(&ev);
	assert(achievements_debug_bad_slot_drops() == bad + 1u);
	puts("PASS malformed events and unaddressable slots are dropped and "
		 "counted");
}

/* R1 LOW 13: EEXIST is not "the directory is there". */
static void test_stats_dir_must_be_a_directory(void) {
	char root[160];
	char stats[200];
	snprintf(root, sizeof(root), "%s/notadir", s_dir);
	make_dir(root);
	snprintf(stats, sizeof(stats), "%s/stats", root);
	FILE* f = fopen(stats, "wb");
	assert(f);
	fputs("not a directory\n", f);
	assert(fclose(f) == 0);

	/* mkdir() returns EEXIST for this, which the old code took as success:
	 * the module then reported itself initialised while every save failed
	 * with ERR_TMP_OPEN forever and nothing said why. */
	assert(!achievements_init(root));
	assert(achievements_init_status() == ACH_INIT_BAD_STATS_DIR);
	assert(achievements_bind(0, 1u, "nodir", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_BAD_KEY);
	/* And the status API says WHY there is no progress tracking. */
	struct ach_status st;
	(void)achievements_status(0, &st);
	assert(st.init_status == (uint8_t)ACH_INIT_BAD_STATS_DIR);
	assert(!st.bound);

	(void)remove(stats);
	assert(achievements_init(s_dir));
	assert(achievements_init_status() == ACH_INIT_OK);
	puts("PASS <root>/stats must be a directory, not merely EEXIST");
}

/* R1 MED 4: the module's state is ONE allocated block, and a failed
 * allocation disables it instead of taking MEM1 unconditionally. */
static void* fail_alloc(size_t n) {
	(void)n;
	return NULL;
}
static void noop_free(void* p) {
	(void)p;
}

/* The MEASURED size of the module's one MEM2 block, pinned (R2 LOW 9).
 * `> 0` was not a test: the memory budget in achievements.c's struct ach_block
 * comment is the thing a reader trusts, and nothing made the two agree -- the
 * overflow ring could double (it did, in F4) and the comment would keep
 * claiming 242,688. The two terms this file CAN compute are spelled out so a
 * ring resize fails here with an arithmetic explanation rather than a bare
 * number, and the per-slot remainder is the measured constant. */
#define CORE_RING_BYTES                                                        \
	((size_t)CW_ACH_RING_CAPACITY * CW_GAME_EVENT_SIZE                         \
	 + (size_t)CW_ACH_OVERFLOW_CAPACITY * CW_GAME_EVENT_SIZE)
#define CORE_SLOT_BYTES 88776u /* B2: generation-tagged folds, +4096 B/slot. */
struct core_save_budget {
 struct stats_profile snapshot;
 struct stats_save_job job;
 bool active;
};
#define CORE_BLOCK_BYTES                                                       \
	(CORE_RING_BYTES + (size_t)CW_ACH_SLOT_COUNT * (CORE_SLOT_BYTES + sizeof(struct core_save_budget)))

static void test_one_block_and_the_no_memory_path(void) {
	size_t bytes = achievements_block_bytes();
	/* The number recorded in achievements.c's struct ach_block comment. If
	 * this ever changes, that comment is what has to change with it. */
	printf("PASS module state block = %lu bytes in ONE allocation (%lu KB; "
		   "%lu ring + 2 x %lu slot + 2 save snapshots), pinned\n",
		   (unsigned long)bytes, (unsigned long)(bytes / 1024u),
		   (unsigned long)CORE_RING_BYTES, (unsigned long)CORE_SLOT_BYTES);
	assert(bytes == CORE_BLOCK_BYTES);

	/* Release the live block so the next init has to go through the
	 * allocator, then make that allocation fail. */
	achievements_shutdown();
	achievements_set_allocator(fail_alloc, noop_free);
	assert(!achievements_init(s_dir));
	assert(achievements_init_status() == ACH_INIT_NO_MEMORY);

	/* Disabled means EMPTY everywhere, and never a crash. */
	struct ach_status st;
	struct ach_notice n;
	assert(!achievements_debug_status(0, &st));
	assert(!achievements_debug_notice(0, &n));
	assert(achievements_debug_profile(0) == NULL);
	assert(achievements_bind(0, 1u, "nomem", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_BAD_KEY);
	assert(!achievements_status(0, &st) && !st.bound);
	assert(st.init_status == (uint8_t)ACH_INIT_NO_MEMORY);
	assert(!achievements_notice_peek(0, &n) && n.ach_id == -1);

	/* Emit is a counted no-op; tick/flush/unbind/clear do nothing. */
	uint32_t drops = achievements_debug_bad_slot_drops();
	emit_simple(GE_JUMPED, GE_AUTH_CLIENT, 0);
	assert(achievements_debug_bad_slot_drops() == drops + 1u);
	achievements_tick();
	achievements_flush_all();
	achievements_notice_clear(0);
	achievements_unbind(0, 1u);

	/* Give the real allocator back: the module comes up again. */
	achievements_set_allocator(NULL, NULL);
	assert(achievements_init(s_dir));
	assert(achievements_init_status() == ACH_INIT_OK);
	assert(achievements_bind(0, s_gen, "backagain", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	emit_simple(GE_JUMPED, GE_AUTH_CLIENT, 0);
	achievements_tick();
	assert(general_of(2010u) == 1u);
	achievements_unbind(0, s_gen);
	puts("PASS the state block is allocated once, and a NULL disables the "
		 "module instead of dropping progress");
}


/* ====================================================================== */
/* F4: the R2 findings that live at the emit boundary                     */
/* ====================================================================== */

/* R2 HIGH 1. The ORDERED class is decided at EMIT TIME from the kind AND the
 * payload, so the ordered share of a normal burst is bounded by Beta's own
 * achievement rules instead of by the kind alone.
 *
 * With the main ring deliberately full, every event takes one of exactly
 * three paths and the status counters say which: the ordered overflow ring
 * (overflow_used), the coalescer (ring_folds), or -- for a pickup Beta keeps
 * no statistic for -- neither (noop_events). Before F4 all three of pickup,
 * craft take and kill were ORDERED for EVERY payload, and a burst of ordinary
 * item pickups inside the supported envelope could fill a ring sized for
 * achievement-rate traffic. */
static void test_emit_time_classification(void) {
	static const struct {
		const char* what;
		uint8_t kind;
		uint8_t authority;
		uint8_t sub;
		uint16_t id;
		int ordered; /* 1 = overflow ring, 0 = coalescer, -1 = no-op */
	} cases[] = {
		/* pickups: only the two achievement ids are ordered; the rest are
		 * no-ops, because Beta has no items-picked-up statistic at all. */
		{"pickup wood 17", GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 0, 17, 1},
		{"pickup leather 334", GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 0, 334, 1},
		{"pickup cobble 4", GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 0, 4, -1},
		{"pickup arrow 262", GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 0, 262, -1},
		/* craft takes: the eight SlotCrafting ids, nothing else. */
		{"craft workbench 58", GE_CRAFT_TAKEN, GE_AUTH_SERVER, 0, 58, 1},
		{"craft cake 354", GE_CRAFT_TAKEN, GE_AUTH_SERVER, 0, 354, 1},
		{"craft planks 5", GE_CRAFT_TAKEN, GE_AUTH_SERVER, 0, 5, 0},
		{"craft stick 280", GE_CRAFT_TAKEN, GE_AUTH_SERVER, 0, 280, 0},
		/* furnace takes: iron and cooked fish. */
		{"smelt iron 265", GE_FURNACE_TAKEN, GE_AUTH_SERVER, 0, 265, 1},
		{"smelt fish 350", GE_FURNACE_TAKEN, GE_AUTH_SERVER, 0, 350, 1},
		{"smelt glass 20", GE_FURNACE_TAKEN, GE_AUTH_SERVER, 0, 20, 0},
		/* kills: the EntityMob set only. */
		{"kill zombie", GE_KILLED, GE_AUTH_SERVER, MONSTER_ZOMBIE, 0, 1},
		{"kill creeper", GE_KILLED, GE_AUTH_SERVER, MONSTER_CREEPER, 0, 1},
		{"kill cow", GE_KILLED, GE_AUTH_SERVER, MONSTER_COW, 0, 0},
		{"kill ghast", GE_KILLED, GE_AUTH_SERVER, MONSTER_GHAST, 0, 0},
		/* the unconditional ones, either way. */
		{"inventory open", GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0, 0, 1},
		{"tick", GE_TICK, GE_AUTH_CLIENT, GE_TICK_NOT_RIDING, 0, 1},
		{"ride end", GE_RIDE_END, GE_AUTH_CLIENT, GE_MOUNT_MINECART, 0, 1},
		{"moved", GE_MOVED, GE_AUTH_CLIENT, GE_MOVE_WALK, 0, 0},
		{"jumped", GE_JUMPED, GE_AUTH_CLIENT, 0, 0, 0},
		{"harvest stone", GE_BLOCK_HARVESTED, GE_AUTH_SERVER, 0, 1, 0},
	};

	for(size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
		fresh_local();
		for(int i = 0; i < CW_ACH_RING_CAPACITY; i++)
			emit_simple(GE_JUMPED, GE_AUTH_CLIENT, 0);
		struct ach_status before = status0();
		struct game_event ev = mk(cases[c].kind, cases[c].authority);
		ev.sub = cases[c].sub;
		ev.u.item.id = cases[c].id;
		ev.u.item.count = 1;
		if(cases[c].kind == GE_MOVED)
			ev.u.move.cm = 5;
		game_event_emit(&ev);
		struct ach_status after = status0();
		if(after.overflow_used - before.overflow_used
			   != (uint32_t)(cases[c].ordered == 1)
		   || after.ring_folds - before.ring_folds
			   != (uint32_t)(cases[c].ordered == 0)
		   || after.noop_events - before.noop_events
			   != (uint32_t)(cases[c].ordered == -1)) {
			fprintf(stderr,
					"FAIL %s: ordered %+d folds %+d noop %+d, wanted class "
					"%d\n",
					cases[c].what,
					(int)(after.overflow_used - before.overflow_used),
					(int)(after.ring_folds - before.ring_folds),
					(int)(after.noop_events - before.noop_events),
					cases[c].ordered);
			assert(0);
		}
		/* A no-op is neither a rejection nor accepted progress. */
		for(int r = 0; r < ACH_REJECT_COUNT; r++)
			assert(after.rejected[r] == before.rejected[r]);
	}

	/* The consequence the finding is actually about: a burst of ordinary item
	 * pickups -- two players x 13 a tick x 10 s in the R2 text -- no longer
	 * touches the ordered ring at all, where before F4 every one of them
	 * queued and 5,200 of them overflowed a ring inside the envelope. */
	fresh_local();
	for(int i = 0; i < CW_ACH_RING_CAPACITY; i++)
		emit_simple(GE_JUMPED, GE_AUTH_CLIENT, 0);
	for(int i = 0; i < 5200; i++)
		emit_item(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 4, 1);
	struct ach_status st = status0();
	assert(st.noop_events == 5200u);
	assert(st.overflow_used == 0u && st.overflow_peak == 0u);
	assert(st.rejected[ACH_REJECT_OVERFLOW] == 0u);
	assert(!st.progress_incomplete);
	achievements_tick();
	assert(status0().accepted == (uint32_t)CW_ACH_RING_CAPACITY);
	puts("PASS the ORDERED class is decided by kind AND payload at emit time, "
		 "and a no-op pickup never queues at all");
}

/* R2 MED 4. Beta nulls startMinecartRidingCoordinate in
 * EntityPlayer.onUpdate, on EVERY tick the player is not riding
 * (EntityPlayer.java:124-127) -- not in addMovementStat. CaveWii reset it on
 * GE_MOVED and GE_RIDE_END, so a player who dismounted without either (a lost
 * RIDE_END, or standing still) kept a stale origin the next ride could cross
 * the onARail threshold against. */
static void test_the_tick_resets_the_rail_origin(void) {
	/* (a) the bug, stated as the test: ride away, lose the dismount, then
	 * mount again far from the old origin and take ONE tick. Without the
	 * reset the first tick of the second ride is 200^2 from the stale origin
	 * and awards onARail from a standing start. */
	fresh_local();
	unlock_ancestors(11);
	emit_ride(GE_MOUNT_MINECART, 0, 0, 0, 5);
	emit_ride(GE_MOUNT_MINECART, 5, 0, 0, 5); /* 25: no award */
	achievements_tick();
	assert(!ach_unlocked(p0(), 11));
	/* No GE_RIDE_END and no GE_MOVED -- the player stood still after
	 * dismounting, which is exactly what Beta still resets on. */
	emit_simple(GE_TICK, GE_AUTH_CLIENT, GE_TICK_NOT_RIDING);
	achievements_tick();
	emit_ride(GE_MOUNT_MINECART, 200, 0, 0, 5); /* the NEW origin */
	emit_ride(GE_MOUNT_MINECART, 205, 0, 0, 5); /* 25 from it: no award */
	achievements_tick();
	assert(!ach_unlocked(p0(), 11));

	/* (b) a RIDING tick does not reset: the rail rule has to survive the
	 * ticks that happen DURING a ride, or onARail could never fire at all. */
	fresh_local();
	unlock_ancestors(11);
	emit_ride(GE_MOUNT_MINECART, 0, 0, 0, 5); /* origin */
	for(int i = 0; i < 4; i++) {
		struct game_event ev = mk(GE_TICK, GE_AUTH_CLIENT);
		ev.sub = (uint8_t)(GE_TICK_RIDING_BASE + GE_MOUNT_MINECART);
		game_event_emit(&ev);
		achievements_tick();
	}
	emit_ride(GE_MOUNT_MINECART, 32, 0, 0, 5); /* 1024 from the origin */
	achievements_tick();
	assert(ach_unlocked(p0(), 11));

	/* (c) GE_MOVED no longer resets anything -- its reset role moved to the
	 * tick -- so a walk between two ride ticks leaves the origin alone. */
	fresh_local();
	unlock_ancestors(11);
	emit_ride(GE_MOUNT_MINECART, 0, 0, 0, 5);
	emit_move(GE_MOVED, GE_AUTH_CLIENT, GE_MOVE_WALK, 7);
	achievements_tick();
	emit_ride(GE_MOUNT_MINECART, 32, 0, 0, 5);
	achievements_tick();
	assert(ach_unlocked(p0(), 11));
	assert(general_of(2000u) == 7u); /* and it still counted the distance */

	/* (d) a tick with a mount kind out of range is malformed, not a reset. */
	fresh_local();
	{
		struct game_event ev = mk(GE_TICK, GE_AUTH_CLIENT);
		ev.sub = (uint8_t)GE_TICK_MOUNT_COUNT;
		game_event_emit(&ev);
	}
	achievements_tick();
	assert(status0().rejected[ACH_REJECT_BAD_EVENT] == 1u);
	assert(general_of(1100u) == 0u);
	puts("PASS a non-riding GE_TICK resets the rail origin (EntityPlayer.java"
		 ":124-127); a riding tick and a GE_MOVED do not");
}

/* R2 MED 6 + MED 7. A coalescer that runs out of keys LOSES a count, so it is
 * a counted rejection with its own reason and a persisted flag -- not a bump
 * of `ring_folds`, which now counts folds that actually happened. */
static void test_coalescer_full_is_counted_and_persisted(void) {
	/* Every event here is a GE_ITEM_BROKEN on an id Beta registers no
	 * breakItem counter for, so the whole window applies NOTHING: the only
	 * reason the slot can end up dirty is the loss itself, which is precisely
	 * what MED 7 says was missing. */
	const uint16_t base = 400u;
	char id[CW_PROFILE_ID_MAX + 1];
	snprintf(id, sizeof(id), "core%d", s_case++);
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, s_gen, id, ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	assert(!status0().dirty);

	for(int i = 0; i < CW_ACH_RING_CAPACITY; i++)
		emit_item(GE_ITEM_BROKEN, GE_AUTH_SERVER, (uint16_t)(base + i), 1);
	assert(status0().ring_folds == 0u && !status0().dirty);

	/* Fill the coalescer to capacity: one distinct key each. */
	for(int i = 0; i < CW_ACH_COALESCE_CAPACITY; i++)
		emit_item(GE_ITEM_BROKEN, GE_AUTH_SERVER,
				  (uint16_t)(base + CW_ACH_RING_CAPACITY + i), 1);
	struct ach_status full = status0();
	assert(full.ring_folds == (uint32_t)CW_ACH_COALESCE_CAPACITY);
	assert(full.coalesce_used == (uint32_t)CW_ACH_COALESCE_CAPACITY);
	assert(full.rejected[ACH_REJECT_COALESCE_FULL] == 0u);
	assert(!full.progress_incomplete && !full.dirty);

	/* One more distinct key has nowhere to go. */
	for(int i = 0; i < 3; i++)
		emit_item(GE_ITEM_BROKEN, GE_AUTH_SERVER,
				  (uint16_t)(base + CW_ACH_RING_CAPACITY
							 + CW_ACH_COALESCE_CAPACITY + i),
				  1);
	struct ach_status over = status0();
	assert(over.rejected[ACH_REJECT_COALESCE_FULL] == 3u);
	/* ring_folds counts FOLDS, not losses (R2 MED 6). */
	assert(over.ring_folds == (uint32_t)CW_ACH_COALESCE_CAPACITY);
	assert(over.progress_incomplete);
	assert(over.dirty); /* R2 MED 7: a loss-only window still has to persist */

	/* And it reaches the FILE, which is the whole point of the flag. */
	achievements_tick();
	achievements_flush_all();
	assert(!status0().dirty);
	assert(achievements_init(s_dir)); /* re-init clears every slot */
	assert(achievements_bind(0, s_gen, id, ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	assert(p0()->progress_incomplete == 1);
	achievements_unbind(0, s_gen);
	puts("PASS a full coalescer is ACH_REJECT_COALESCE_FULL, not a fold, and "
		 "the loss-only window persists progress_incomplete");
}

/* The same rule on the ORDERED side: an overflow rejection is the only thing
 * that happens in the window, and it still has to reach the file (R2 MED 7).
 * Every queued event is a GE_RIDE_END, which resets an origin and touches no
 * counter, so nothing else can mark the slot dirty. */
static void test_overflow_rejection_persists_progress_incomplete(void) {
	char id[CW_PROFILE_ID_MAX + 1];
	snprintf(id, sizeof(id), "core%d", s_case++);
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, s_gen, id, ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);

	for(int i = 0; i < CW_ACH_RING_CAPACITY + CW_ACH_OVERFLOW_CAPACITY; i++)
		emit_simple(GE_RIDE_END, GE_AUTH_CLIENT, GE_MOUNT_MINECART);
	assert(!status0().dirty && !status0().progress_incomplete);
	emit_simple(GE_RIDE_END, GE_AUTH_CLIENT, GE_MOUNT_MINECART);
	struct ach_status st = status0();
	assert(st.rejected[ACH_REJECT_OVERFLOW] == 1u);
	assert(st.progress_incomplete && st.dirty);

	achievements_tick();
	achievements_flush_all();
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, s_gen, id, ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);
	assert(p0()->progress_incomplete == 1);
	achievements_unbind(0, s_gen);
	puts("PASS an ordered-overflow rejection marks the slot dirty, so the "
		 "loss reaches the file header");
}

int main(void) {
	test_fixture_dir("test_achievements_core_tmp", s_dir, sizeof(s_dir));
	make_dir(s_dir);

	test_all_sixteen_triggers();
	test_non_triggers();
	test_parent_locked_drops();
	test_repeat_counts_without_second_toast();
	test_toast_content_and_window();
	test_notice_clear_leaves_the_hint();
	test_pig_fold_awards_per_qualifying_fall();
	test_accepted_counts_only_what_was_applied();
	test_item_counters_use_the_block_alias();
	test_plain_counters();
	test_malformed_events_are_counted();
	test_emit_time_classification();
	test_the_tick_resets_the_rail_origin();
	test_coalescer_full_is_counted_and_persisted();
	test_overflow_rejection_persists_progress_incomplete();
	test_stats_dir_must_be_a_directory();
	test_one_block_and_the_no_memory_path();

	achievements_shutdown();
	test_fixture_dir_cleanup(s_dir);
	puts("PASS test_achievements_core");
	return 0;
}
