/* Copyright (c) 2026 CaveWii_v2 contributors */
/* docs/achievements_design.md section 9, row `test_achievements_owner`:
 *
 *   two bound slots: events to slot 1 never touch slot 0; stale generation
 *   dropped and counted; unbound slot dropped; bad key / duplicate profile /
 *   busy slot rejected; unbind drains, flushes, clears ride origin and
 *   notice; a rebind with a new generation never sees old events;
 *   notice_peek(0) never returns the notice of slot 1
 *
 * The notice assertions go through B2's internal accessor rather than B4's
 * achievements_notice_peek(), which does not exist yet; the state they read
 * is the same one B4 will expose.
 *
 * The bind-time `mode` is also the thing that picks the counter column, so
 * the 5.2 acceptance matrix is exercised here at one case per row. The full
 * per-row provenance golden is brief B3's test_stats_provenance.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "support/test_fixture_dir.h"

#include "entity/monsters_object.h"
#include "game/achievements/achievements.h"
#include "platform/thread.h"

static char s_dir[128];
static int s_case;
/* One resident scratch profile (~57 KB): static, never on the stack. */
static struct stats_profile g_reload;

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

static void emit_jump(uint8_t slot, uint64_t generation) {
	struct game_event ev = mk(GE_JUMPED, GE_AUTH_CLIENT, slot, generation);
	game_event_emit(&ev);
}

static void emit_item(uint8_t kind, uint8_t authority, uint8_t slot,
					  uint64_t generation, uint16_t id, uint16_t count) {
	struct game_event ev = mk(kind, authority, slot, generation);
	ev.u.item.id = id;
	ev.u.item.count = count;
	game_event_emit(&ev);
}

static void emit_ride(uint8_t slot, uint64_t generation, int32_t x, int32_t y,
					  int32_t z) {
	struct game_event ev = mk(GE_RIDE, GE_AUTH_CLIENT, slot, generation);
	ev.sub = GE_MOUNT_MINECART;
	ev.u.ride.x = x;
	ev.u.ride.y = y;
	ev.u.ride.z = z;
	ev.u.ride.cm = 10;
	game_event_emit(&ev);
}

static void emit_remote(uint8_t slot, uint64_t generation, int32_t stat_id,
						int32_t amount) {
	struct game_event ev
		= mk(GE_REMOTE_STAT, GE_AUTH_PACKET200, slot, generation);
	ev.u.remote.stat_id = stat_id;
	ev.u.remote.amount = amount;
	game_event_emit(&ev);
}

static struct ach_status status_of(uint8_t slot) {
	struct ach_status st;
	memset(&st, 0, sizeof(st));
	assert(achievements_debug_status(slot, &st));
	return st;
}

static uint32_t general_of(uint8_t slot, uint32_t stat_id) {
	int i = stat_general_index(stat_id);
	assert(i >= 0);
	return stats_counter_total(
		&achievements_debug_profile(slot)->general[i]);
}

static void next_ids(char* a, char* b, size_t n) {
	snprintf(a, n, "own%da", s_case);
	snprintf(b, n, "own%db", s_case);
	s_case++;
}

/* ------------------------------------------------------------------ cases */

static void test_bind_rules(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));

	/* Invalid keys [design 3]: NULL, empty, too long, out-of-charset. */
	assert(achievements_bind(0, 1u, NULL, ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_BAD_KEY);
	assert(achievements_bind(0, 1u, "", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_BAD_KEY);
	assert(achievements_bind(0, 1u, "seventeen_chars_x", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_BAD_KEY);
	assert(achievements_bind(0, 1u, "Player1", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_BAD_KEY);
	assert(achievements_bind(0, 1u, "bad-key", ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_BAD_KEY);
	/* A slot that does not exist is not an identity either. */
	assert(achievements_bind(CW_ACH_SLOT_COUNT, 1u, a, ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_BAD_KEY);

	assert(achievements_bind(0, 1u, a, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	/* Same slot again, before unbind. */
	assert(achievements_bind(0, 2u, b, ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_BUSY);
	/* The other slot, same profile: two players never share one file. */
	assert(achievements_bind(1, 2u, a, ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_DUPLICATE);
	assert(achievements_bind(1, 2u, b, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);

	/* A released profile may be bound on the other slot afterwards. */
	achievements_unbind(0, 1u);
	assert(achievements_bind(0, 3u, b, ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_DUPLICATE); /* b is still on slot 1 */
	assert(achievements_bind(0, 3u, a, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	puts("PASS bind rules: bad key, duplicate profile, busy slot");
}

static void test_slot_isolation(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 10u, a, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	assert(achievements_bind(1, 20u, b, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);

	for(int i = 0; i < 3; i++)
		emit_jump(1, 20u);
	emit_jump(0, 10u);
	/* Slot 1 unlocks openInventory; slot 0 does not. */
	struct game_event ev
		= mk(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 1, 20u);
	game_event_emit(&ev);
	achievements_tick();

	assert(general_of(0, 2010u) == 1u);
	assert(general_of(1, 2010u) == 3u);
	assert(!ach_unlocked(achievements_debug_profile(0), 0));
	assert(ach_unlocked(achievements_debug_profile(1), 0));

	/* The toast belongs to the slot that earned it and to nothing else -- and
	 * so does the hint: slot 0 still has openInventory locked, slot 1 does
	 * not. */
	struct ach_notice n0, n1;
	memset(&n0, 0x5A, sizeof(n0));
	assert(!achievements_debug_notice(0, &n0));
	assert(n0.ach_id == -1 && n0.hint_active);
	assert(achievements_debug_notice(1, &n1));
	assert(n1.ach_id == 0 && !n1.hint_active);
	puts("PASS per-slot isolation: counters, unlocks, the toast and the hint");
}

static void test_stale_and_unbound(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 100u, a, ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);

	emit_jump(0, 101u); /* a previous player's generation */
	achievements_tick();
	assert(general_of(0, 2010u) == 0u);
	assert(status_of(0).rejected[ACH_REJECT_STALE_GENERATION] == 1u);

	/* Slot 1 was never bound. */
	emit_jump(1, 1u);
	achievements_tick();
	assert(status_of(1).rejected[ACH_REJECT_UNBOUND] == 1u);
	assert(general_of(1, 2010u) == 0u);
	puts("PASS stale generations and unbound slots are dropped and counted");
}

static void test_unbind_drains_flushes_clears(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 5u, a, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);

	/* Unlock openInventory (queues a notice) and set a ride origin, then
	 * leave a batch of events queued WITHOUT draining. */
	struct game_event ev = mk(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0, 5u);
	game_event_emit(&ev);
	emit_ride(0, 5u, 0, 0, 0);
	achievements_tick();
	assert(ach_unlocked(achievements_debug_profile(0), 0));
	struct ach_notice n;
	assert(achievements_debug_notice(0, &n) && n.ach_id == 0);

	for(int i = 0; i < 4; i++)
		emit_jump(0, 5u);
	assert(general_of(0, 2010u) == 0u); /* still queued, not drained */

	achievements_unbind(0, 5u);

	/* (1) it drained: the queued jumps were applied before the release. */
	assert(general_of(0, 2010u) == 4u);
	/* (2) it flushed: the file on disk carries them. */
	enum stats_load_status ls = STATS_LOAD_CORRUPT;
	stats_profile_load(test_fixture_join(s_dir, "/stats"), a, &g_reload, &ls);
	assert(ls == STATS_LOAD_OK);
	int jump = stat_general_index(2010u);
	assert(stats_counter_total(&g_reload.general[jump]) == 4u);
	assert(ach_unlocked(&g_reload, 0));
	/* (3) it cleared the notice, and left the slot neither bound nor closing. */
	assert(!achievements_debug_notice(0, &n));
	assert(n.ach_id == -1 && !n.hint_active);
	assert(!status_of(0).bound && !status_of(0).closing);

	/* (4) it cleared the ride origin: after a rebind, the FIRST mounted move
	 * sets a fresh origin, so one far-away ride cannot unlock onARail even
	 * though the pre-unbind origin was 1000+ away. */
	assert(achievements_bind(0, 6u, a, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	/* The reload restored openInventory, but onARail's parent is
	 * acquireIron, so walk the rest of the spine up to it first. */
	emit_item(GE_ITEM_PICKED_UP, GE_AUTH_SERVER, 0, 6u, 17, 1);
	emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 0, 6u, 58, 1);
	emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 0, 6u, 270, 1);
	emit_item(GE_CRAFT_TAKEN, GE_AUTH_SERVER, 0, 6u, 61, 1);
	emit_item(GE_FURNACE_TAKEN, GE_AUTH_SERVER, 0, 6u, 265, 1);
	achievements_tick();
	assert(ach_unlocked(achievements_debug_profile(0), 5));
	emit_ride(0, 6u, 40, 40, 40); /* would be >= 1000 from the old origin */
	achievements_tick();
	assert(!ach_unlocked(achievements_debug_profile(0), 11));
	emit_ride(0, 6u, 0, 0, 0); /* 40^2*3 = 4800 from the NEW origin */
	achievements_tick();
	assert(ach_unlocked(achievements_debug_profile(0), 11));
	puts("PASS unbind drains, flushes, clears the notice and the ride origin");
}

static void test_rebind_never_sees_old_events(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 41u, a, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	achievements_unbind(0, 41u);

	/* The previous player's producer is still running for one frame. The slot
	 * is UNBOUND at that instant, and with the F4 binding gate (R2 LOW 13) an
	 * emit is refused there and then rather than queued to be re-checked at
	 * the drain -- so the reason is UNBOUND and it is readable NOW, before the
	 * bind below clears the slot's diagnostics. */
	for(int i = 0; i < 3; i++)
		emit_jump(0, 41u);
	assert(status_of(0).rejected[ACH_REJECT_UNBOUND] == 3u);
	/* The slot is reused by somebody else, with a new generation. */
	assert(achievements_bind(0, 42u, b, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	achievements_tick();

	assert(general_of(0, 2010u) == 0u);
	/* Nothing survived the bind to be re-judged at the drain. */
	assert(status_of(0).rejected[ACH_REJECT_STALE_GENERATION] == 0u);
	/* An unbind with the wrong generation changes nothing. */
	achievements_unbind(0, 41u);
	assert(status_of(0).bound);
	assert(status_of(0).rejected[ACH_REJECT_STALE_GENERATION] == 1u);
	puts("PASS a rebind with a new generation never sees the old player's "
		 "events");
}

/* R2 LOW 13. The emit path used to check the binding ONLY when the main ring
 * was full, on the argument that a ring entry carries its generation and the
 * drain re-checks it. That is true of a STALE generation and false of a
 * FUTURE one: an event emitted BEFORE the bind, carrying the generation the
 * bind is about to use, sat in the ring and then passed actor_ok() at the next
 * drain -- so a producer that ran one frame early credited the new player with
 * a fact from before they existed, which is exactly the leak
 * test_rebind_never_sees_old_events() exists to forbid in the other
 * direction. */
static void test_a_pre_bind_emit_never_lands_after_the_bind(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));

	/* Nothing is bound on slot 0, and these carry the generation the bind is
	 * ABOUT to use -- the one case a drain-time check cannot catch. */
	for(int i = 0; i < 5; i++)
		emit_jump(0, 77u);
	/* Two ordered kinds as well, so this covers the achievement path and not
	 * just a counter: an inventory open would unlock openInventory outright. */
	{
		struct game_event ev = mk(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 0, 77u);
		game_event_emit(&ev);
		game_event_emit(&ev);
	}
	assert(status_of(0).rejected[ACH_REJECT_UNBOUND] == 7u);

	assert(achievements_bind(0, 77u, a, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	achievements_tick();
	/* Not one of the seven was applied, and the ring is empty rather than
	 * holding them for a later drain. */
	assert(general_of(0, 2010u) == 0u);
	assert(!ach_unlocked(achievements_debug_profile(0), 0));
	assert(status_of(0).accepted == 0u);
	assert(status_of(0).notices_queued == 0u);
	/* The slot works normally afterwards: this is a gate, not a wedge. */
	emit_jump(0, 77u);
	achievements_tick();
	assert(general_of(0, 2010u) == 1u);
	achievements_unbind(0, 77u);
	(void)b;
	puts("PASS an emit that arrives before the bind is refused at emit, even "
		 "carrying the generation the bind will use");
}

static void test_mode_picks_the_column_and_the_matrix(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));
	/* Slot 0 is in a local world, slot 1 is on a remote server [design 6]. */
	assert(achievements_bind(0, 1u, a, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	assert(achievements_bind(1, 1u, b, ACH_MODE_REMOTE_SERVER)
		   == ACH_BIND_OK);

	/* LOCAL: server-owned from the server thread, client-owned from the
	 * client -- both accepted into the LOCAL column. */
	emit_jump(0, 1u);
	emit_item(GE_BLOCK_HARVESTED, GE_AUTH_SERVER, 0, 1u, 1, 1);
	/* LOCAL: the two wrong-observer rows, and Packet 200 with no remote
	 * server at all. */
	emit_item(GE_BLOCK_HARVESTED, GE_AUTH_CLIENT, 0, 1u, 1, 1);
	struct game_event ev = mk(GE_JUMPED, GE_AUTH_SERVER, 0, 1u);
	game_event_emit(&ev);
	emit_remote(0, 1u, 2022, 1);
	achievements_tick();

	const struct stats_profile* p = achievements_debug_profile(0);
	int jump = stat_general_index(2010u);
	assert(p->general[jump].local == 1u && p->general[jump].remote == 0u);
	assert(p->mine[1].local == 1u && p->mine[1].remote == 0u);
	struct ach_status s0 = status_of(0);
	assert(s0.rejected[ACH_REJECT_WRONG_OBSERVER] == 2u);
	assert(s0.rejected[ACH_REJECT_NO_REMOTE_SERVER] == 1u);

	/* REMOTE: a client-owned fact still counts locally (Beta's
	 * EntityClientPlayerMP.addStat), a server-owned one arrives as Packet
	 * 200, and an achievement over Packet 200 goes through the same parent
	 * check -- all into the REMOTE column. */
	emit_jump(1, 1u);
	emit_remote(1, 1u, 2022, 3);						  /* deaths, server */
	emit_remote(1, 1u, (int32_t)CW_ACH_BASE, 1);		  /* openInventory */
	emit_remote(1, 1u, (int32_t)(CW_ACH_BASE + 1), 1);	  /* mineWood */
	/* REMOTE rejections: a client-owned id over Packet 200 (Beta's
	 * func_27027_b rejects those too), a server-owned fact claimed by the
	 * client, and anything at all from a local server that is not running. */
	emit_item(GE_BLOCK_HARVESTED, GE_AUTH_CLIENT, 1, 1u, 1, 1);
	ev = mk(GE_JUMPED, GE_AUTH_SERVER, 1, 1u);
	game_event_emit(&ev);
	achievements_tick();

	p = achievements_debug_profile(1);
	assert(p->general[jump].remote == 1u && p->general[jump].local == 0u);
	int deaths = stat_general_index(2022u);
	assert(p->general[deaths].remote == 3u);
	/* openInventory is Beta's one INDEPENDENT (client-owned) achievement, so
	 * a server that sends it over Packet 200 is rejected -- which also means
	 * mineWood behind it is dropped for a locked parent. */
	assert(!ach_unlocked(p, 0));
	assert(!ach_unlocked(p, 1));
	struct ach_status s1 = status_of(1);
	assert(s1.rejected[ACH_REJECT_WRONG_OBSERVER] == 2u);
	assert(s1.rejected[ACH_REJECT_NO_LOCAL_SERVER] == 1u);
	/* R2 LOW 11: the parent-locked drop is counted in its own status field,
	 * not as a rejection. */
	assert(s1.ach_dropped_parent_locked == 1u);

	/* The client's own openInventory DOES count on a remote server, and then
	 * the server-sent mineWood lands. */
	ev = mk(GE_INVENTORY_OPENED, GE_AUTH_CLIENT, 1, 1u);
	game_event_emit(&ev);
	emit_remote(1, 1u, (int32_t)(CW_ACH_BASE + 1), 2);
	achievements_tick();
	assert(ach_unlocked(p, 0) && ach_unlocked(p, 1));
	assert(p->ach[1].remote == 2u && p->ach[1].local == 0u);
	assert(p->ach_first[1] == (uint8_t)STATS_FIRST_REMOTE);

	/* An id this build has no counter for is counted as unknown, not
	 * silently absorbed. */
	uint32_t unknown = status_of(1).rejected[ACH_REJECT_UNKNOWN_STAT];
	emit_remote(1, 1u, 4242, 1);
	emit_remote(1, 1u, (int32_t)(CW_STAT_MINE_BASE + 18), 1); /* leaves */
	achievements_tick();
	assert(status_of(1).rejected[ACH_REJECT_UNKNOWN_STAT] == unknown + 2u);
	puts("PASS the bind mode picks the column and drives the 5.2 matrix");
}

static void test_flush_all(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 1u, a, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	assert(achievements_bind(1, 1u, b, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	emit_jump(0, 1u);
	emit_jump(1, 1u);
	emit_jump(1, 1u);
	achievements_flush_all(); /* drains, then writes both files */

	assert(!status_of(0).dirty && !status_of(1).dirty);
	assert(status_of(0).save_failures == 0u
		   && status_of(1).save_failures == 0u);
	int jump = stat_general_index(2010u);
	enum stats_load_status ls = STATS_LOAD_CORRUPT;
	stats_profile_load(test_fixture_join(s_dir, "/stats"), a, &g_reload, &ls);
	assert(ls == STATS_LOAD_OK
		   && stats_counter_total(&g_reload.general[jump]) == 1u);
	stats_profile_load(test_fixture_join(s_dir, "/stats"), b, &g_reload, &ls);
	assert(ls == STATS_LOAD_OK
		   && stats_counter_total(&g_reload.general[jump]) == 2u);
	puts("PASS flush_all drains and writes every dirty bound profile");
}

/* ---- R1 MED 8: no event may be discarded by a release without a counter --
 *
 * The producer emits FLAT OUT for the whole duration of the release and stops
 * only once it has returned, so the window the fix exists for -- between the
 * drain and the final coalescer_clear() -- is guaranteed to be crossed. The
 * claim is an exact identity: every event it emitted is either applied or
 * counted under a reason. */
#define RACE_GEN 900u

static volatile uint32_t s_race_go;
static volatile uint32_t s_race_stop;
static volatile uint32_t s_race_emitted;

static void* unbind_race_producer(void* arg) {
	(void)arg;
	uint32_t n = 0;
	__atomic_store_n(&s_race_go, 1u, __ATOMIC_RELEASE);
	while(!__atomic_load_n(&s_race_stop, __ATOMIC_ACQUIRE)) {
		emit_jump(0, RACE_GEN);
		n++;
	}
	__atomic_store_n(&s_race_emitted, n, __ATOMIC_RELEASE);
	return NULL;
}

static void test_unbind_loses_nothing_uncounted(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, RACE_GEN, a, ACH_MODE_LOCAL_WORLD)
		   == ACH_BIND_OK);

	__atomic_store_n(&s_race_go, 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&s_race_stop, 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&s_race_emitted, 0u, __ATOMIC_RELEASE);
	struct thread th;
	assert(thread_create(&th, unbind_race_producer, NULL, 64));
	while(!__atomic_load_n(&s_race_go, __ATOMIC_ACQUIRE))
		thread_yield();

	achievements_unbind(0, RACE_GEN);

	__atomic_store_n(&s_race_stop, 1u, __ATOMIC_RELEASE);
	thread_join(&th);
	/* Whatever the producer queued after the release is still sitting in the
	 * ring; draining it is what turns it into a counted UNBOUND rejection. */
	achievements_tick();

	uint32_t emitted = __atomic_load_n(&s_race_emitted, __ATOMIC_ACQUIRE);
	struct ach_status st = status_of(0);
	uint64_t seen = st.accepted;
	for(int r = 0; r < ACH_REJECT_COUNT; r++)
		seen += st.rejected[r];
	assert(emitted > 0u);
	assert(seen == (uint64_t)emitted); /* THE CLAIM: nothing vanished */
	/* The closing window was really crossed, so the identity above is not
	 * passing by never reaching the code it is about. */
	assert(st.rejected[ACH_REJECT_CLOSING] > 0u
		   || st.rejected[ACH_REJECT_UNBOUND] > 0u);
	assert(!st.progress_incomplete && !st.bound && !st.closing);
	/* Everything the release accepted is on the counter, too. */
	assert(general_of(0, 2010u) == st.accepted);
	printf("PASS unbind loses nothing uncounted: %u emitted = %u accepted + %u "
		   "closing + %u unbound\n",
		   (unsigned)emitted, (unsigned)st.accepted,
		   (unsigned)st.rejected[ACH_REJECT_CLOSING],
		   (unsigned)st.rejected[ACH_REJECT_UNBOUND]);
}

/* ---- R1 MED 7: a status snapshot can never see a half-updated pair -------
 *
 * The observer snapshots in a loop, asserting on every one of them that
 * (save_failures, last_save_error) is one of the two CONSISTENT states. The
 * barrier runs inside the failed flush, under the module lock, between the
 * two writes: while it holds that lock the observer must make no progress at
 * all -- which is exactly what the old version, writing those fields with no
 * lock at all, could not promise. */
static volatile uint32_t s_snap_stop;
static volatile uint32_t s_snaps;
static struct ach_status s_snap_last;

static void* status_observer(void* arg) {
	(void)arg;
	while(!__atomic_load_n(&s_snap_stop, __ATOMIC_ACQUIRE)) {
		struct ach_status st;
		memset(&st, 0, sizeof(st));
		if(!achievements_debug_status(0, &st))
			continue;
		assert((st.save_failures == 0u && st.last_save_error == 0u)
			   || (st.save_failures == 1u
				   && st.last_save_error == (uint8_t)STATS_SAVE_ERR_WRITE));
		assert(st.dirty); /* a failing flush never clears it, before or after */
		s_snap_last = st;
		__atomic_add_fetch(&s_snaps, 1u, __ATOMIC_RELEASE);
	}
	return NULL;
}

static void status_barrier(void) {
	/* We hold the module lock. The observer is either blocked on it or about
	 * to be, so it cannot complete a snapshot for as long as we stay here. */
	uint32_t before = __atomic_load_n(&s_snaps, __ATOMIC_ACQUIRE);
	thread_msleep(20);
	/* The observer bumps s_snaps AFTER achievements_debug_status() released
	 * the lock, so exactly one snapshot that completed just before we took
	 * the lock may still be counted during our hold (reproduced 3/40 under
	 * six busy loops, 2026-09-14, F1 review). With a single observer that
	 * straggler is the only one possible: any snapshot STARTED after
	 * `before` blocks on the lock we hold and cannot complete. So the sound
	 * claim is "advanced by at most one", not "unchanged". */
	assert(__atomic_load_n(&s_snaps, __ATOMIC_ACQUIRE) - before <= 1u);
}

static void test_status_snapshot_never_tears(void) {
	char a[CW_PROFILE_ID_MAX + 1], b[CW_PROFILE_ID_MAX + 1];
	next_ids(a, b, sizeof(a));
	assert(achievements_init(s_dir));
	assert(achievements_bind(0, 1u, a, ACH_MODE_LOCAL_WORLD) == ACH_BIND_OK);
	emit_jump(0, 1u);
	achievements_tick();
	assert(status_of(0).dirty);

	__atomic_store_n(&s_snap_stop, 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&s_snaps, 0u, __ATOMIC_RELEASE);
	struct thread th;
	assert(thread_create(&th, status_observer, NULL, 64));

	achievements_debug_status_barrier(status_barrier);
	stats_io_fail_at(STATS_IO_STAGE_WRITE);
	achievements_flush_all();

	__atomic_store_n(&s_snap_stop, 1u, __ATOMIC_RELEASE);
	thread_join(&th);
	assert(__atomic_load_n(&s_snaps, __ATOMIC_ACQUIRE) > 0u);

	struct ach_status st = status_of(0);
	assert(st.save_failures == 1u);
	assert(st.last_save_error == (uint8_t)STATS_SAVE_ERR_WRITE);
	assert(st.dirty);
	/* The last snapshot the observer managed to take is a complete state,
	 * never the torn one. */
	assert(s_snap_last.save_failures == 0u
		   || s_snap_last.last_save_error == (uint8_t)STATS_SAVE_ERR_WRITE);
	/* The retry succeeds and the history stays on record. */
	achievements_flush_all();
	st = status_of(0);
	assert(!st.dirty && st.save_failures == 1u);
	achievements_unbind(0, 1u);
	puts("PASS a status snapshot taken during a flush never shows a "
		 "half-updated pair");
}

int main(void) {
	test_fixture_dir("test_achievements_owner_tmp", s_dir, sizeof(s_dir));
	make_dir(s_dir);

	test_bind_rules();
	test_slot_isolation();
	test_stale_and_unbound();
	test_unbind_drains_flushes_clears();
	test_rebind_never_sees_old_events();
	test_a_pre_bind_emit_never_lands_after_the_bind();
	test_mode_picks_the_column_and_the_matrix();
	test_flush_all();
	test_unbind_loses_nothing_uncounted();
	test_status_snapshot_never_tears();

	achievements_shutdown();
	test_fixture_dir_cleanup(s_dir);
	puts("PASS test_achievements_owner");
	return 0;
}
