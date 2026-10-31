/* Copyright (c) 2026 CaveWii_v2 contributors */
/*
 * Ring + coalescer + acceptance matrix + the sixteen Beta unlock rules +
 * bind/unbind/flush [docs/achievements_design.md 1.1, 1.2, 3, 5.1, 5.2, 6].
 *
 * Shape of the file, top to bottom:
 *   1. module state (the two rings, the per-slot state, the one mutex)
 *   2. table helpers derived from stats_table.h (owner, achievement index,
 *      the ORDERED/FOLDABLE class)
 *   3. the 5.2 acceptance matrix
 *   4. the coalescer
 *   5. game_event_emit() -- the only thing producers touch
 *   6. the drain: apply a fold, the sixteen rules, the notice
 *   7. lifecycle: init / bind / unbind / tick / flush
 *
 * LOCKING. There is exactly ONE mutex, and it covers both rings, every
 * coalescer and every per-slot status/profile/notice. game_event_emit() takes
 * it for a few dozen instructions; the drain takes it for the whole apply
 * pass. Holding it across the apply pass is deliberate and safe:
 *   - the apply path is pure arithmetic over fixed arrays -- no allocation,
 *     no I/O, and it never calls game_event_emit(), so it cannot re-enter;
 *   - it is bounded (<= 1024 main-ring + <= 1024 overflow-ring entries,
 *     + <= 512 folds per slot);
 *   - the alternative (double-buffering every coalescer so the drain can work
 *     outside the lock) costs another ~36 KB of MEM1 to shave microseconds
 *     off a producer that is, by construction, already running ahead of a
 *     stalled drain.
 * File I/O is the one thing that is NEVER done under the lock: flushes run
 * after the drain has released it -- but every write to a slot's `status` is
 * back under it, including the two fields that record a flush result, so a
 * snapshot can never catch that pair half updated (R1 MED 7).
 */

/* Strict -std=c11 -pedantic hides mkdir/stat's POSIX declarations unless they
 * are requested before any system header; the same preamble
 * achievements_persist.c and source/network/complex_block_archive.c use. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <sys/types.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "../../entity/monsters_object.h"
#include "../../platform/mem2.h"
#include "../../platform/thread.h"
#include "../../platform/time.h"
#include "achievements.h"

/* ---------------------------------------------------------------- 1. state */

/* Power of two >= 2 * CW_ACH_COALESCE_CAPACITY so linear probing stays short
 * even at capacity. */
#define CW_ACH_COALESCE_BUCKETS 1024
#define CW_ACH_BUCKET_EMPTY ((int16_t)-1)

/* ONE APPLIED UNIT. The drain has exactly one apply path, and it takes this
 * struct from two places:
 *
 *  - a RING entry (main ring or ordered overflow ring): built by
 *    fold_from_event() with count == 1 and the event's own payload, including
 *    the GE_RIDE coordinates the rail rule needs. Exact, in emit order.
 *  - a COALESCER entry: (kind, sub, authority, id) -> how many FOLDABLE events
 *    carried that key and their summed payload. B2 also keys by epoch:
 *    48-byte entries retain new-owner counters until BEGIN can be consumed.
 *
 * `id` is 32 bits because GE_REMOTE_STAT's key is a whole Beta stat id
 * (0x1030000 + item id at the top end), not a 16-bit item id.
 *
 * `amount` folds ADDITIVELY, always -- there is no longer any other fold rule
 * [design 5.2 v4]. The two exceptions the v2 coalescer carried are gone with
 * the kinds that needed them: GE_PIG_FELL_WITH_RIDER (a THRESHOLD payload that
 * folded as a MAX plus a `qualifying` count) and GE_RIDE{minecart} (which kept
 * only the LATEST coordinates, so a cart that crossed the onARail threshold
 * and came back inside it inside one stalled window was not seen) are both
 * ORDERED now and can never reach the coalescer. What folds is a pure counter,
 * and a sum of counters is exact.
 *
 * `ride_x/y/z` and `fall` are therefore written ONLY by fold_from_event(); a
 * coalescer entry leaves them zero, which drain_locked() asserts. `fall` is
 * the pig-fall distance in BLOCKS as a float, because EntityPig.fall tests
 * `f > 5F` on the float itself (R2 MED 5) -- there is no integer scale on
 * which that predicate is the same predicate. */
struct ach_fold {
	uint64_t generation; /* folding must never cross a lifecycle boundary */
	uint8_t kind;
	uint8_t sub;
	uint8_t authority;
	uint8_t used;
	uint32_t id;
	uint64_t amount;
	uint32_t count;
	int32_t ride_x, ride_y, ride_z; /* ring path only (GE_RIDE) */
	float fall;						/* ring path only (GE_PIG_FELL_WITH_RIDER) */
};

struct ach_coalescer {
	struct ach_fold e[CW_ACH_COALESCE_CAPACITY];
	int16_t bucket[CW_ACH_COALESCE_BUCKETS];
	uint16_t used;
	bool overflow; /* copied onto the profile at the next drain */
};

struct ach_slot {
	struct stats_profile profile;
	struct ach_status status;
	struct ach_notice notice;
	struct ach_notice notices[CW_ACH_COUNT];
	uint8_t notice_head, notice_count;
	struct ach_coalescer co;
	/* EntityPlayer.startMinecartRidingCoordinate [design 1.1 row 11]. */
	bool ride_origin_valid;
	int32_t ride_x, ride_y, ride_z;
};

/* THE MODULE'S ONE BLOCK [design 5.2 "Memory placement", R1 MED 4].
 *
 * The ring and the two slot states used to be static .bss: ~194 KB of MEM1
 * claimed unconditionally at link time, dormant until lane A emits its first
 * event, in an engine with a documented history of booting to a black screen
 * when .bss grew (PROJECT_MEMORY: "boot-death pad = relocation", a 188 KB
 * main() frame against a 128 KB stack). MEM1 is the scarce pool -- framebuffers,
 * the GX FIFO, display lists -- and none of this is GX-read or latency-critical,
 * so it belongs in the 64 MB MEM2 pool that sits ~75% idle.
 *
 * ONE block, not three allocations: the ring and both slots have exactly the
 * same lifetime (init to shutdown), so splitting them would only add two more
 * chances to fail and two more holes in the carve's free list.
 *
 * mem2_alloc() is the engine's existing large-block allocator (mem2.h): the
 * MEM2 carve on Wii -- falling back to the ordinary heap, never a hard failure
 * -- and a plain 32-byte-aligned malloc on PC. NULL disables the module rather
 * than dropping progress silently.
 *
 * SIZE, printed AND PINNED by tests/test_achievements_core.c (the
 * achievements_block_bytes() case, so the number is measured and not claimed):
 *
 *	 425,664 bytes total (R2: immutable save snapshots, measured on PC)
 *	 =  40,960 main ring     (CW_ACH_RING_CAPACITY 1024 x 40-byte game_event)
 *	 +  81,920 overflow ring (CW_ACH_OVERFLOW_CAPACITY 2048, same entry type)
 *	 + 2 x 88,776 slot       (61,792 stats_profile + 26,624 coalescer
 *	                          + status + diagnostic notice + notice FIFO
 *	                          + ride origin)
 *	 + 2 x 62,616 save state (snapshot + cooperative writer + active flag)
 *
 * The overflow ring doubled to 2048 with F4: v3 sized it against a rate the
 * envelope did not actually bound (R2 HIGH 1), and the per-class ORDERED
 * envelope that replaced it is 1,040 events for two players across a 10 s
 * stall -- 2048 is twice that. Neither ring is a dial. The exact total is
 * MEASURED, not claimed: test_achievements_core prints
 * achievements_block_bytes() and pins it against the constant computed from
 * the same ring, slot and save terms, so this comment cannot drift from the layout. */
struct ach_save {
	struct stats_profile snapshot;
	struct stats_save_job job;
	bool active;
};

struct ach_block {
	struct game_event ring[CW_ACH_RING_CAPACITY];
	/* Ordered events that arrived with the main ring full [design 5.2 v4].
	 * ONE FIFO across both slots, exactly like the main ring, so a slot's
	 * events keep their relative order wherever the split lands. */
	struct game_event overflow[CW_ACH_OVERFLOW_CAPACITY];
	struct ach_slot slot[CW_ACH_SLOT_COUNT];
	struct ach_save save[CW_ACH_SLOT_COUNT];
};

static struct thread_mutex s_lock;
static bool s_lock_ready;
static bool s_inited;

static struct ach_block* s_blk;
static void* (*s_alloc)(size_t);
static void (*s_free)(void*);
static uint8_t s_init_status = (uint8_t)ACH_INIT_NEVER_RUN;

/* The block's two halves, named as the rest of the file already named them.
 * Never dereferenced unless s_blk != NULL, which s_inited guarantees. */
#define s_ring (s_blk->ring)
#define s_ovf (s_blk->overflow)
#define s_slot (s_blk->slot)

static uint32_t s_ring_head;
static uint32_t s_ring_count;
static uint32_t s_ovf_head;
static uint32_t s_ovf_count;

static char s_stats_dir[CW_STATS_PATH_MAX];
static ptime_t s_epoch;
static uint32_t s_bad_slot_drops;  /* slot outside 0..CW_ACH_SLOT_COUNT-1 */
static uint32_t s_pre_init_drops;  /* emitted before achievements_init() */

/* Admission identity is published under the same lock as emit. The consumer
 * may still be saving the preceding owner; new facts wait behind BEGIN in
 * the FIFO. Folds retain the epoch whose profile must receive them. Eight reserved
 * entries keep two-owner close/open/checkpoint facts out of gameplay bursts. */
#define ACH_CONTROL_RESERVE 8u
static struct {
	uint64_t epoch;
	uint64_t checkpoint;
	bool managed;
	bool incomplete;
	bool blocked, retry;
	uint8_t mode;
} s_route[CW_ACH_SLOT_COUNT];
static char s_profiles[CW_ACH_SLOT_COUNT][CW_PROFILE_ID_MAX + 1];
static bool s_profiles_valid, s_cooperative_saves;
static unsigned s_control_overflow;
static bool s_save_requested[CW_ACH_SLOT_COUNT], s_save_error_ack[CW_ACH_SLOT_COUNT];
static uint32_t s_save_serial[CW_ACH_SLOT_COUNT];
static uint32_t s_coalescer_rebuilds;
static bool s_in_tick, s_tick_save_attempted[CW_ACH_SLOT_COUNT];
uint32_t achievements_debug_coalescer_rebuilds(void) { return s_coalescer_rebuilds; }
static uint64_t s_next_epoch;

static bool lifecycle_event(const struct game_event* ev) {
	return ev->kind >= GE_SESSION_BEGIN;
}

/* Caller holds s_lock. An admitted epoch may still be in stats_profile_load
 * on main: producers must never mutate that profile, even on overflow. */
static void mark_incomplete(struct ach_slot* s, unsigned slot) {
	s->status.progress_incomplete = true;
	if(s_route[slot].managed) s_route[slot].incomplete = true;
	else {
		s->profile.progress_incomplete = 1;
		s->status.dirty = true;
	}
}

uint64_t game_event_next_epoch(void) {
	if(!s_lock_ready) return 0;
	thread_mutex_lock(&s_lock);
	uint64_t epoch = s_next_epoch == UINT64_MAX ? 0 : ++s_next_epoch;
	thread_mutex_unlock(&s_lock);
	return epoch;
}

uint64_t game_event_owner_epoch(uint8_t slot) {
	if(slot >= CW_ACH_SLOT_COUNT || !s_lock_ready) return 0;
	thread_mutex_lock(&s_lock);
	uint64_t epoch = s_route[slot].epoch;
	thread_mutex_unlock(&s_lock);
	return epoch;
}

bool achievements_set_profiles(const char* player1, const char* player2) {
	if(!s_lock_ready) return false;
	thread_mutex_lock(&s_lock);
	bool valid = stats_profile_id_valid(player1)
		&& stats_profile_id_valid(player2) && strcmp(player1, player2) != 0;
	/* Startup only: changing an admitted owner's key is never implicit. */
	if(s_route[0].managed || s_route[1].managed) valid = false;
	if(valid) {
		snprintf(s_profiles[0], sizeof(s_profiles[0]), "%s", player1);
		snprintf(s_profiles[1], sizeof(s_profiles[1]), "%s", player2);
	}
	s_profiles_valid = valid;
	thread_mutex_unlock(&s_lock);
	return valid;
}

bool achievements_lifecycle_pending(void) {
	if(!s_lock_ready) return false;
	thread_mutex_lock(&s_lock);
	bool pending = s_route[0].blocked || s_route[1].blocked || s_control_overflow;
	thread_mutex_unlock(&s_lock);
	return pending;
}

/* Milliseconds since achievements_init(), OFFSET so the clock never starts
 * inside a notice window. Only differences are ever meaningful (the toast
 * window of design 1.3), so the zero point is free -- and starting it at one
 * full window means the backdated info hint can always be expressed without
 * clamping at 0, including on the very first frame after init. */
static uint32_t module_now_ms(void) {
	int32_t d = time_diff_ms(time_get(), s_epoch);
	if(d < 0)
		d = 0;
	return ACH_NOTICE_WINDOW_MS + (uint32_t)d;
}

/* ------------------------------------------------------- 2. table helpers */

/* Beta owner flag of a general stat row, straight out of the X-macro so the
 * matrix can never disagree with stats_table.h [design 1.4]. */
static int stat_general_owner(int index) {
	int cursor = 0;
#define CW_STAT(sid, key, type, owner)                                         \
	if(index == cursor)                                                        \
		return CW_STAT_OWNER_##owner;                                          \
	cursor++;
	CW_STAT_ROWS
#undef CW_STAT
	(void)cursor;
	return CW_STAT_OWNER_SERVER;
}

/* CW_ACH_BASE + i -> i, else -1. */
static int ach_index_of_stat_id(uint32_t id) {
	if(id < CW_ACH_BASE || id >= CW_ACH_BASE + (uint32_t)CW_ACH_COUNT)
		return -1;
	return (int)(id - CW_ACH_BASE);
}

/* Which class a whole-stat id belongs to, for GE_REMOTE_STAT. False means
 * "this build has no counter for that id" (an unknown id, or a block whose
 * Beta registration was suppressed) -- the caller counts UNKNOWN_STAT. */
static bool stat_owner_of_id(uint32_t id, int* owner) {
	int a = ach_index_of_stat_id(id);
	if(a >= 0) {
		/* field_27088_g: openInventory alone is client-owned [design 1.2
		 * point 4], which is exactly what makes the func_27027_b rejection
		 * of a client-owned id over Packet 200 observable. */
		*owner = ach_is_independent(a) ? CW_STAT_OWNER_CLIENT
									   : CW_STAT_OWNER_SERVER;
		return true;
	}
	int g = stat_general_index(id);
	if(g >= 0) {
		*owner = stat_general_owner(g);
		return true;
	}
	if(id >= CW_STAT_MINE_BASE
	   && id < CW_STAT_MINE_BASE + (uint32_t)CW_STAT_BLOCK_COUNT) {
		if(stat_canonical_block((uint16_t)(id - CW_STAT_MINE_BASE))
		   == CW_STAT_BLOCK_NONE)
			return false;
		*owner = CW_STAT_OWNER_SERVER;
		return true;
	}
	{
		/* The three item arrays. A BLOCK id in one of them is gated the same
		 * way the mine array is (F3): Beta registers no craftItem counter for
		 * an uncraftable block and no breakItem counter for any block at all,
		 * so StatList.func_27361_a() hands the Packet 200 handler a null stat
		 * and addStat returns early -- this build has no counter for that id,
		 * which is exactly what `false` means here. Item ids are ungated. */
		enum cw_stat_array which;
		uint32_t base;
		if(id >= CW_STAT_CRAFT_BASE && id < CW_STAT_CRAFT_BASE + ITEMS_MAX) {
			which = CW_STAT_ARRAY_CRAFT;
			base = CW_STAT_CRAFT_BASE;
		} else if(id >= CW_STAT_USE_BASE && id < CW_STAT_USE_BASE + ITEMS_MAX) {
			which = CW_STAT_ARRAY_USE;
			base = CW_STAT_USE_BASE;
		} else if(id >= CW_STAT_BREAK_BASE
				  && id < CW_STAT_BREAK_BASE + ITEMS_MAX) {
			which = CW_STAT_ARRAY_BREAK;
			base = CW_STAT_BREAK_BASE;
		} else {
			return false;
		}
		if(stat_alias_block_in(which, (uint16_t)(id - base))
		   == CW_STAT_BLOCK_NONE)
			return false;
		*owner = CW_STAT_OWNER_SERVER;
		return true;
	}
}

/* Which class an event kind belongs to [design 1.4 "Beta owner" column, read
 * through the 5.3 site map]. GE_REMOTE_STAT is not here: its class comes from
 * the stat id it carries. */
static int kind_owner(uint8_t kind) {
	switch(kind) {
		case GE_JUMPED:
		case GE_DROPPED:
		case GE_INVENTORY_OPENED:
		case GE_MOVED:
		case GE_FELL:
		case GE_RIDE:
		case GE_RIDE_END:
		case GE_GAME_STARTED:
		case GE_GAME_LEFT:
		case GE_MP_JOINED:
		case GE_TICK: return CW_STAT_OWNER_CLIENT;
		default: return CW_STAT_OWNER_SERVER;
	}
}

/* EntityPlayer.onKillEntity fires killEnemy only for an EntityMob victim
 * [design 1.1 row 13]: zombie, skeleton, creeper, spider, pig zombie, giant.
 * NOT ghast (EntityFlying), NOT slime (EntityLiving), NOT wolf/animals. */
static bool mob_is_hostile_for_kill_enemy(uint8_t sub) {
	switch(sub) {
		case MONSTER_ZOMBIE:
		case MONSTER_SKELETON:
		case MONSTER_CREEPER:
		case MONSTER_SPIDER:
		case MONSTER_PIG_ZOMBIE:
		case MONSTER_GIANT: return true;
		default: return false;
	}
}

/* SlotCrafting.onPickupFromSlot's else-if chain, in AchievementList
 * registration order [design 1.1]. The ids are distinct, so the chain's
 * precedence is unobservable in Beta too -- it is written as a chain because
 * that is what the decompile does and because a future duplicate id would
 * then behave exactly as Beta would. */
static int craft_achievement_for(uint32_t item_id) {
	if(item_id == 58)
		return 2; /* buildWorkBench */
	else if(item_id == 270)
		return 3; /* buildPickaxe */
	else if(item_id == 61)
		return 4; /* buildFurnace */
	else if(item_id == 290)
		return 6; /* buildHoe */
	else if(item_id == 297)
		return 7; /* makeBread */
	else if(item_id == 354)
		return 8; /* bakeCake */
	else if(item_id == 274)
		return 9; /* buildBetterPickaxe */
	else if(item_id == 268)
		return 12; /* buildSword */
	return -1;
}

/* ---- ORDERED vs FOLDABLE [design 5.2 v4, Astra call 2 + R2 HIGH 1] -------
 *
 * One class per kind, built straight from CW_EVENT_CLASS_ROWS so the table in
 * stats_table.h is the only place the classification exists, plus the payload
 * refinements that table documents but cannot express.
 *
 * This replaces the v2 `fold_rank()` ordering entirely. That function existed
 * because the coalescer could hold achievement-bearing kinds and had to drain
 * them in AchievementList order so a prerequisite still landed before its
 * dependant -- an ordering that is right for the parent graph and wrong for
 * everything else (it also reordered a GE_RIDE against the GE_MOVED that
 * should have reset its origin). With every achievement-bearing kind on the
 * ordered path, the coalescer holds pure counters, there is nothing left to
 * rank, and emit order is preserved instead of reconstructed. */
static const uint8_t s_event_class[GE_KIND_COUNT] = {
#define CW_EVENT_CLASS(kind, cls) [kind] = (uint8_t)CW_EVENT_CLASS_##cls,
	CW_EVENT_CLASS_ROWS
#undef CW_EVENT_CLASS
};

/* Every kind has exactly one class: a new kind cannot be added to
 * game_event.h without adding its row to stats_table.h. */
_Static_assert((int)CW_EVENT_CLASS_COUNT == (int)GE_KIND_COUNT,
			   "every game_event kind needs an ORDERED/FOLDABLE class row");

/* Can THIS EVENT -- this kind with THIS PAYLOAD -- change an achievement or a
 * notice? Then it must be applied in emit order and may never reach the
 * coalescer.
 *
 * The payload half is the R2 HIGH 1 fix. A per-KIND table has to answer for
 * the whole kind, so it answered ORDERED for every pickup, every craft take
 * and every kill; none of those three is bounded by the design's
 * 64-per-player-per-tick envelope, so the supported envelope could produce
 * thousands of ordered events and overflow a ring sized for hundreds. Beta
 * only ever reads a handful of payloads per kind, and every other payload is a
 * pure counter whose order cannot matter -- so the refinement is not a
 * heuristic, it is the achievement rules of design 1.1 read backwards. */
static bool event_is_ordered(const struct game_event* ev) {
	if(ev->kind >= GE_KIND_COUNT)
		return false; /* rejected as malformed before this ever matters */
	if(s_event_class[ev->kind] != CW_EVENT_CLASS_ORDERED)
		return false;
	switch(ev->kind) {
		/* EntityItem.onCollideWithPlayer fires mineWood for wood (17) and
		 * killCow for leather (334) and nothing else [design 1.1 rows 1, 14].
		 * Any other pickup is a no-op, not a counter -- see event_is_noop(). */
		case GE_ITEM_PICKED_UP:
			return ev->u.item.id == 17u || ev->u.item.id == 334u;
		/* SlotCrafting.onPickupFromSlot's else-if chain [design 1.1]. Every
		 * other take is a pure craftItem[] count. */
		case GE_CRAFT_TAKEN: return craft_achievement_for(ev->u.item.id) >= 0;
		/* SlotFurnace checks iron and cooked fish [design 1.1 rows 5, 10]. */
		case GE_FURNACE_TAKEN:
			return ev->u.item.id == 265u || ev->u.item.id == 350u;
		/* EntityPlayer.onKillEntity fires killEnemy for an EntityMob victim
		 * [design 1.1 row 13]; every other kill is a mobKills counter. */
		case GE_KILLED: return mob_is_hostile_for_kill_enemy(ev->sub);
		/* The onARail origin / squared-1000 rule; a boat or pig ride is a
		 * pure distance counter. */
		case GE_RIDE: return ev->sub == GE_MOUNT_MINECART;
		/* func_27027_b funnels an achievement id through the parent check and
		 * the toast; every other id is a pure counter. */
		case GE_REMOTE_STAT:
			return ach_index_of_stat_id((uint32_t)ev->u.remote.stat_id) >= 0;
		default: return true;
	}
}

/* Is this event one Beta keeps NO statistic for at all, so that applying it
 * would be a no-op on every counter and every achievement?
 *
 * Exactly one kind can be: a pickup of anything but wood or leather. Beta has
 * no "items picked up" statistic (design 1.4 has no such row), so
 * EntityItem.onCollideWithPlayer's only statistical effect is the two
 * achievements above. Such an event is dropped AT EMIT, before it consumes a
 * ring slot or a coalescer key, and counted in ach_status.noop_events --
 * which is not a rejection: nothing was lost, there was nothing to keep
 * (R2 HIGH 1). Keeping them queued is what let a normal item burst threaten
 * the ordered ring. */
static bool event_is_noop(const struct game_event* ev) {
	return ev->kind == GE_ITEM_PICKED_UP && ev->u.item.id != 17u
		&& ev->u.item.id != 334u;
}

/* --------------------------------------------------------- 3. the matrix */

/* docs/achievements_design.md 5.2. `authority` says who OBSERVED the fact,
 * `mode` says which world the session is in, `owner` is the Beta class of the
 * statistic. Anything not in the table is a rejection with a reason. */
static bool matrix_accept(enum ach_bind_mode mode, uint8_t authority, int owner,
						  enum stats_column* column,
						  enum ach_reject_reason* why) {
	if(mode == ACH_MODE_LOCAL_WORLD) {
		if(authority == GE_AUTH_PACKET200) {
			*why = ACH_REJECT_NO_REMOTE_SERVER;
			return false;
		}
		if((authority == GE_AUTH_SERVER && owner == CW_STAT_OWNER_SERVER)
		   || (authority == GE_AUTH_CLIENT && owner == CW_STAT_OWNER_CLIENT)) {
			*column = STATS_COLUMN_LOCAL;
			return true;
		}
		*why = ACH_REJECT_WRONG_OBSERVER;
		return false;
	}
	/* ACH_MODE_REMOTE_SERVER: no local server runs. */
	if(authority == GE_AUTH_SERVER) {
		*why = ACH_REJECT_NO_LOCAL_SERVER;
		return false;
	}
	if((authority == GE_AUTH_CLIENT && owner == CW_STAT_OWNER_CLIENT)
	   || (authority == GE_AUTH_PACKET200 && owner == CW_STAT_OWNER_SERVER)) {
		*column = STATS_COLUMN_REMOTE;
		return true;
	}
	*why = ACH_REJECT_WRONG_OBSERVER;
	return false;
}

/* ------------------------------------------------------ 4. the coalescer */

/* The payload that folds: what the counters add per event. Clamped at 0 --
 * a negative payload is rejected at emit, so this never sees one. */
static uint64_t payload_amount(const struct game_event* ev) {
	switch(ev->kind) {
		case GE_ITEM_PICKED_UP:
		case GE_CRAFT_TAKEN:
		case GE_FURNACE_TAKEN: return (uint64_t)ev->u.item.count;
		case GE_MOVED:
		case GE_FELL: return (uint64_t)ev->u.move.cm;
		/* No counter exists for a pig fall: the payload is the float in
		 * u.fall, carried through struct ach_fold::fall, and `amount` is only
		 * ever the one event this fold stands for. */
		case GE_PIG_FELL_WITH_RIDER: return 1u;
		case GE_RIDE: return (uint64_t)ev->u.ride.cm;
		case GE_DAMAGE_DEALT:
		case GE_DAMAGE_TAKEN: return (uint64_t)ev->u.n.amount;
		case GE_REMOTE_STAT: return (uint64_t)ev->u.remote.amount;
		default: return 1u;
	}
}

/* The id half of the coalescer key. */
static uint32_t fold_key_id(const struct game_event* ev) {
	switch(ev->kind) {
		case GE_BLOCK_HARVESTED:
		case GE_CRAFT_TAKEN:
		case GE_FURNACE_TAKEN:
		case GE_ITEM_PICKED_UP:
		case GE_ITEM_USED:
		case GE_ITEM_BROKEN: return (uint32_t)ev->u.item.id;
		case GE_REMOTE_STAT: return (uint32_t)ev->u.remote.stat_id;
		default: return 0u;
	}
}

static uint32_t fold_hash(uint8_t kind, uint8_t sub, uint8_t authority,
						  uint32_t id) {
	uint32_t h = 2166136261u;
	h = (h ^ kind) * 16777619u;
	h = (h ^ sub) * 16777619u;
	h = (h ^ authority) * 16777619u;
	h = (h ^ (id & 0xFFu)) * 16777619u;
	h = (h ^ ((id >> 8) & 0xFFu)) * 16777619u;
	h = (h ^ ((id >> 16) & 0xFFu)) * 16777619u;
	h = (h ^ ((id >> 24) & 0xFFu)) * 16777619u;
	return h;
}

/* EntityPig.fall's predicate, ON THE FLOAT Beta compares [EntityPig.java:112,
 * `if(f > 5F && (riddenByEntity instanceof EntityPlayer))`]. Exactly 5.0
 * blocks does NOT fire.
 *
 * It takes the float and not a x100 integer because the two are not the same
 * predicate (R2 MED 5): a fall of 5.004 blocks is `> 5F` in Beta and rounds to
 * 500 centimetres here, which `> 500` rejects. Every value in (5.0, 5.005]
 * diverged. */
static bool pig_fall_qualifies(float f) {
	return f > 5.0F;
}

/* A FOLDABLE payload is a pure counter, so the fold is a saturating SUM and
 * nothing else [design 5.2 v4]. No MAX, no latest-wins field, no predicate. */
static void fold_accumulate(struct ach_fold* f, const struct game_event* ev) {
	f->count++;
	uint64_t amount = payload_amount(ev);
	if(f->amount > UINT64_MAX - amount)
		f->amount = UINT64_MAX;
	else
		f->amount += amount;
}

/* Returns false when the coalescer is full -- the one case the design calls
 * "not reachable inside the envelope" and treats as a test failure -- or when
 * an ORDERED kind reached it, which is a module bug: the emit path routes
 * those to the overflow ring. The assert fires in every test build (the suite
 * is forced -UNDEBUG); the return is the release-build backstop, which the
 * caller turns into a counted rejection plus progress_incomplete rather than a
 * silent approximation [design 5.2 v4]. */
static bool coalesce_fold(struct ach_coalescer* c,
						  const struct game_event* ev) {
	assert(!event_is_ordered(ev));
	if(event_is_ordered(ev)) {
		c->overflow = true; /* same loud path as a full coalescer */
		return false;
	}
	uint32_t id = fold_key_id(ev);
	uint32_t slot
		= fold_hash(ev->kind, ev->sub, ev->authority, id)
		^ (uint32_t)ev->who.generation ^ (uint32_t)(ev->who.generation >> 32);
	slot &= (uint32_t)(CW_ACH_COALESCE_BUCKETS - 1);
	for(int probe = 0; probe < CW_ACH_COALESCE_BUCKETS; probe++) {
		int16_t at = c->bucket[slot];
		if(at == CW_ACH_BUCKET_EMPTY) {
			if(c->used >= CW_ACH_COALESCE_CAPACITY) {
				c->overflow = true;
				return false;
			}
			at = (int16_t)c->used++;
			c->bucket[slot] = at;
			struct ach_fold* f = &c->e[at];
			memset(f, 0, sizeof(*f));
			f->generation = ev->who.generation;
			f->kind = ev->kind;
			f->sub = ev->sub;
			f->authority = ev->authority;
			f->id = id;
			f->used = 1;
			fold_accumulate(f, ev);
			return true;
		}
		struct ach_fold* f = &c->e[at];
		if(f->generation == ev->who.generation && f->kind == ev->kind && f->sub == ev->sub
		   && f->authority == ev->authority && f->id == id) {
			fold_accumulate(f, ev);
			return true;
		}
		slot = (slot + 1u) & (uint32_t)(CW_ACH_COALESCE_BUCKETS - 1);
	}
	c->overflow = true;
	return false;
}

static void coalescer_clear(struct ach_coalescer* c) {
	c->used = 0;
	c->overflow = false;
	memset(c->bucket, 0xFF, sizeof(c->bucket)); /* 0xFFFF == -1 */
}

/* ---------------------------------------------------------- 5. the emit */

/* Is this event well-formed? Checked ONCE, at emit, so the drain can trust
 * every range and so a producer bug is counted against the slot that caused
 * it instead of surfacing as a mystery later. */
static bool event_well_formed(const struct game_event* ev) {
	if(ev->kind >= GE_KIND_COUNT || ev->authority >= GE_AUTH_COUNT)
		return false;
	switch(ev->kind) {
		case GE_MOVED:
			if(ev->sub >= GE_MOVE_KIND_COUNT || ev->u.move.cm < 0)
				return false;
			break;
		case GE_FELL:
			if(ev->u.move.cm < 0)
				return false;
			break;
		case GE_PIG_FELL_WITH_RIDER:
			/* A fall distance in blocks: never negative, and written as
			 * `!(f >= 0)` so a NaN payload is malformed too rather than
			 * silently failing every later compare. */
			if(!(ev->u.fall.f >= 0.0F))
				return false;
			break;
		case GE_RIDE:
			if(ev->sub >= GE_MOUNT_KIND_COUNT || ev->u.ride.cm < 0)
				return false;
			break;
		case GE_RIDE_END:
			if(ev->sub >= GE_MOUNT_KIND_COUNT)
				return false;
			break;
		case GE_TICK:
			/* sub is enum game_event_tick_mount: 0 = not riding (the faithful
			 * rail-origin reset, EntityPlayer.java:124-127), otherwise
			 * GE_TICK_RIDING_BASE + the mount kind. */
			if(ev->sub >= GE_TICK_MOUNT_COUNT)
				return false;
			break;
		case GE_GAME_STARTED:
			if(ev->sub >= GE_WORLD_KIND_COUNT)
				return false;
			break;
		case GE_KILLED:
			if(ev->sub >= MONSTER_COUNT)
				return false;
			break;
		case GE_DAMAGE_DEALT:
		case GE_DAMAGE_TAKEN:
			if(ev->u.n.amount < 0)
				return false;
			break;
		case GE_REMOTE_STAT:
			/* design 5.1: GE_REMOTE_STAT is ALWAYS Packet 200. */
			if(ev->authority != GE_AUTH_PACKET200 || ev->u.remote.amount < 0
			   || ev->u.remote.stat_id < 0)
				return false;
			break;
		default: break;
	}
	return true;
}

void game_event_emit(const struct game_event* ev) {
	if(!ev)
		return;
	if(!__atomic_load_n(&s_inited, __ATOMIC_ACQUIRE)) {
		/* Not a per-slot rejection: without init there is no slot state to
		 * count it in, and the mutex may not exist yet. */
		__atomic_add_fetch(&s_pre_init_drops, 1u, __ATOMIC_RELAXED);
		return;
	}
	if(ev->who.slot >= CW_ACH_SLOT_COUNT) {
		__atomic_add_fetch(&s_bad_slot_drops, 1u, __ATOMIC_RELAXED);
		return;
	}

	thread_mutex_lock(&s_lock);
	if(!s_blk) {
		/* achievements_shutdown() won the race after the s_inited load above.
		 * There is no per-slot state left to count it in. */
		thread_mutex_unlock(&s_lock);
		__atomic_add_fetch(&s_pre_init_drops, 1u, __ATOMIC_RELAXED);
		return;
	}
	struct ach_slot* s = &s_slot[ev->who.slot];
	if(lifecycle_event(ev)) {
		bool begin = ev->kind == GE_SESSION_BEGIN;
		bool valid = ev->who.generation && ev->authority != GE_AUTH_PACKET200
			&& ev->authority < GE_AUTH_COUNT
			&& (!begin || (s_profiles_valid && ev->sub <= ACH_MODE_REMOTE_SERVER));
		if(!valid || (!begin && s_route[ev->who.slot].epoch != ev->who.generation)
		   || (begin && s_route[ev->who.slot].epoch)) {
			s->status.rejected[ACH_REJECT_STALE_GENERATION]++;
			thread_mutex_unlock(&s_lock);
			return;
		}
		if(ev->kind != GE_SESSION_END) s_route[ev->who.slot].retry = true;
		/* Saves are barriers, not counted gameplay facts. An outstanding
		 * checkpoint already covers this epoch's next main-thread drain. */
		if(ev->kind == GE_SESSION_CHECKPOINT
		   && s_route[ev->who.slot].checkpoint == ev->who.generation) {
			thread_mutex_unlock(&s_lock);
			return;
		}
		if(!s_ovf_count && s_ring_count < CW_ACH_RING_CAPACITY) {
			s_ring[(s_ring_head + s_ring_count++) % CW_ACH_RING_CAPACITY] = *ev;
		} else if(s_ovf_count < CW_ACH_OVERFLOW_CAPACITY) {
			s_ovf[(s_ovf_head + s_ovf_count++) % CW_ACH_OVERFLOW_CAPACITY] = *ev;
		} else {
			/* Outside the supported lifecycle envelope: never overwrite an
			 * earlier boundary. Main tick reconciles the latest admitted route;
			 * lost facts stay explicitly incomplete on disk. */
			s_control_overflow |= 1u << ev->who.slot;
			s_route[ev->who.slot].incomplete = true;
			s_route[ev->who.slot].managed = true;
			if(begin) { s_route[ev->who.slot].epoch = ev->who.generation; s_route[ev->who.slot].mode = ev->sub; }
			else if(ev->kind == GE_SESSION_END) s_route[ev->who.slot].epoch = 0;
			s->status.progress_incomplete = true;
			thread_mutex_unlock(&s_lock);
			return;
		}
		s_route[ev->who.slot].managed = true;
		if(begin) { s_route[ev->who.slot].epoch = ev->who.generation; s_route[ev->who.slot].mode = ev->sub; }
		else if(ev->kind == GE_SESSION_END) s_route[ev->who.slot].epoch = 0;
		else s_route[ev->who.slot].checkpoint = ev->who.generation;
		thread_mutex_unlock(&s_lock);
		return;
	}
	if(!event_well_formed(ev)) {
		s->status.rejected[ACH_REJECT_BAD_EVENT]++;
		thread_mutex_unlock(&s_lock);
		return;
	}
	/* THE CLOSING GATE (R1 MED 8). achievements_unbind() sets this under this
	 * same lock and drains without releasing it, so from that instant nothing
	 * can enter the ring OR the coalescer for this slot -- which is what makes
	 * the release's coalescer_clear() provably empty and every racing emit
	 * COUNTED rather than discarded. Checked before the ring so the ordered
	 * path is gated too, not just the fold. */
	if(!s_route[ev->who.slot].managed && s->status.closing) {
		s->status.rejected[(s->status.generation == ev->who.generation)
							   ? ACH_REJECT_CLOSING
							   : ACH_REJECT_STALE_GENERATION]++;
		thread_mutex_unlock(&s_lock);
		return;
	}
	/* THE BINDING GATE, for EVERY path (R2 LOW 13). It used to guard only the
	 * two full-ring paths, on the argument that the coalescer has no room for
	 * a generation while a ring entry carries one and can be re-checked at
	 * drain. That is true of a STALE generation and false of a FUTURE one: an
	 * emit that arrives before the bind, carrying the generation the bind is
	 * about to use, sat in the ring and passed actor_ok() at the next drain --
	 * so a producer that emitted one frame early credited the new player with
	 * an event from before they existed. Checked here, under the lock, the
	 * ring can only hold the admitted route's epoch. B2's queued BEGIN is an
	 * explicit admission, so that epoch may wait for its main-thread bind;
	 * the drain's actor_ok() remains the ownership backstop. */
	bool managed = s_route[ev->who.slot].managed;
	uint64_t epoch = managed ? s_route[ev->who.slot].epoch : s->status.generation;
	if(managed ? !epoch : !s->status.bound) {
		s->status.rejected[ACH_REJECT_UNBOUND]++;
		thread_mutex_unlock(&s_lock);
		return;
	}
	if(epoch != ev->who.generation) {
		s->status.rejected[ACH_REJECT_STALE_GENERATION]++;
		thread_mutex_unlock(&s_lock);
		return;
	}
	/* NOTHING TO KEEP: Beta has no statistic this event could reach, so it is
	 * dropped before it takes a ring slot (R2 HIGH 1). Counted, but not as a
	 * rejection and not as accepted. */
	if(event_is_noop(ev)) {
		s->status.noop_events++;
		thread_mutex_unlock(&s_lock);
		return;
	}
	unsigned reserve = managed ? ACH_CONTROL_RESERVE : 0;
	if(!s_ovf_count && s_ring_count < CW_ACH_RING_CAPACITY - reserve) {
		s_ring[(s_ring_head + s_ring_count) % CW_ACH_RING_CAPACITY] = *ev;
		s_ring_count++;
		thread_mutex_unlock(&s_lock);
		return;
	}
	/* MAIN RING FULL: the ordered / foldable split. */
	if(event_is_ordered(ev)) {
		/* It can change an achievement or a notice, so it is NEVER folded
		 * and never approximated [design 5.2 v4]. Same FIFO discipline as the
		 * main ring, so this slot's events keep their relative emit order
		 * across the split: the main ring only ever fills (it does not drain
		 * mid-burst), so once an event lands here every later one does too. */
		if(s_ovf_count < CW_ACH_OVERFLOW_CAPACITY - reserve) {
			s_ovf[(s_ovf_head + s_ovf_count) % CW_ACH_OVERFLOW_CAPACITY] = *ev;
			s_ovf_count++;
			s->status.overflow_used++;
			if(s->status.overflow_used > s->status.overflow_peak)
				s->status.overflow_peak = s->status.overflow_used;
		} else {
			/* Outside the envelope. LOUD: a counted reason and a flag that
			 * reaches the file header and the screens -- never a silent fold
			 * of an order-sensitive fact. `dirty` because the flag is only
			 * useful if it is PERSISTED (R2 MED 7): without it a window that
			 * lost events but applied none left the slot clean, so the header
			 * bit was never written and the next load said progress was
			 * complete. */
			s->status.rejected[ACH_REJECT_OVERFLOW]++;
			mark_incomplete(s, ev->who.slot);
		}
		thread_mutex_unlock(&s_lock);
		return;
	}
	/* FOLDABLE: a pure counter, so the order it lands in cannot matter. */
	if(coalesce_fold(&s->co, ev)) {
		s->status.ring_folds++;
		if(s->co.used > s->status.coalesce_peak)
			s->status.coalesce_peak = s->co.used;
		s->status.coalesce_used = s->co.used;
	} else {
		/* The coalescer is full (or an ORDERED kind reached it, which is a
		 * module bug the assert in coalesce_fold() catches first). The count
		 * is LOST, so it is a counted rejection with its own reason and a
		 * persisted flag -- NOT a bump of `ring_folds`, which now counts folds
		 * that actually happened and nothing else (R2 MED 6). */
		s->status.rejected[ACH_REJECT_COALESCE_FULL]++;
		mark_incomplete(s, ev->who.slot);
	}
	thread_mutex_unlock(&s_lock);
}

/* --------------------------------------------------------- 6. the drain */

static void add_u64(struct stats_pair* p, enum stats_column column,
					uint64_t v) {
	/* The counter itself saturates at UINT32_MAX [design 2], so clamping the
	 * addend first produces the identical result without a 64-bit counter. */
	stats_counter_add(p, column, (v > (uint64_t)UINT32_MAX) ? UINT32_MAX
														   : (uint32_t)v);
}

static void add_general(struct ach_slot* s, uint32_t stat_id,
						enum stats_column column, uint64_t v) {
	int idx = stat_general_index(stat_id);
	if(idx < 0)
		return;
	add_u64(&s->profile.general[idx], column, v);
	s->status.dirty = true;
}

/* Approved per-owner FIFO (Brief E2). Each achievement unlocks once per
 * binding, so CW_ACH_COUNT retains the entire chain, even behind a menu.
 * The persistent hint remains separate from these unlock notices. */
static void (*s_notice_hook)(uint8_t slot, int16_t ach_id);

static void notice_set(struct ach_slot* s, int ach_id, uint32_t now_ms) {
	assert(s->notice_count < CW_ACH_COUNT);
	unsigned tail = (s->notice_head + s->notice_count) % CW_ACH_COUNT;
	s->notices[tail] = (struct ach_notice) {
		.ach_id = (int16_t)ach_id, .started_ms = now_ms};
	/* Preserve the B2 diagnostic's most-recent-unlock observation. It is not
	 * the public FIFO front (Brief E2); no progress/counters change here. */
	s->notice.ach_id = (int16_t)ach_id;
	s->notice.started_ms = now_ms;
	s->notice_count++;
	s->status.notices_queued++;
	/* Runtime-armed test observer, still under the existing module lock. */
	if(s_notice_hook)
		s_notice_hook((uint8_t)(s - s_slot), (int16_t)ach_id);
}

/* EntityPlayerSP.onLivingUpdate shows the openInventory hint for exactly as
 * long as the achievement is locked [design 1.1 row 0]. Re-evaluated wherever
 * that can change: at bind (from the loaded profile), on a GE_TICK, and the
 * moment the achievement is awarded. */
static void hint_refresh(struct ach_slot* s) {
	s->notice.hint_active = !ach_unlocked(&s->profile, 0);
}

/* EntityPlayerSP.addStat for an achievement [design 1.2]:
 *   1. parent locked -> the event is DROPPED ENTIRELY (not counted, not
 *      deferred);
 *   2. otherwise queue the toast IF it was still locked, then count -- every
 *      repeat increments, only the first queues a toast.
 * Returns whether anything was APPLIED, so the caller knows not to count a
 * dropped event as accepted (R1 LOW 15). */
static bool ach_award(struct ach_slot* s, int i, uint64_t increment,
					  enum stats_column column, uint32_t now_ms) {
	if(i < 0 || i >= CW_ACH_COUNT)
		return false;
	if(!ach_can_unlock(&s->profile, i)) {
		/* Beta drops the achievement half and nothing else; the event may
		 * still have applied a counter, and counting it as a REJECTION while
		 * also counting it as accepted put one event on both sides of the
		 * ledger (R2 LOW 11). This is its own status counter, not a
		 * rejected[] reason: the rule worked. */
		uint32_t add = (increment > (uint64_t)UINT32_MAX) ? UINT32_MAX
														  : (uint32_t)increment;
		if(s->status.ach_dropped_parent_locked > UINT32_MAX - add)
			s->status.ach_dropped_parent_locked = UINT32_MAX;
		else
			s->status.ach_dropped_parent_locked += add;
		return false;
	}
	if(increment == 0)
		return false;
	bool was_unlocked = ach_unlocked(&s->profile, i);
	if(!was_unlocked)
		notice_set(s, i, now_ms);
	add_u64(&s->profile.ach[i], column, increment);
	if(!was_unlocked && s->profile.ach_first[i] == (uint8_t)STATS_FIRST_NONE)
		s->profile.ach_first[i] = (uint8_t)((column == STATS_COLUMN_REMOTE)
												? STATS_FIRST_REMOTE
												: STATS_FIRST_LOCAL);
	if(i == 0)
		hint_refresh(s); /* openInventory just unlocked: the hint is done */
	s->status.dirty = true;
	return true;
}

static void ride_origin_reset(struct ach_slot* s) {
	s->ride_origin_valid = false;
	s->ride_x = s->ride_y = s->ride_z = 0;
}

/* EntityPlayer.addMountedMovementStat's rail rule [design 1.1 row 11, 5.1]:
 * the origin is set on the first mounted move after a reset and NOT checked
 * on that same event (Beta's `else if`); every later one compares the SQUARED
 * integer block distance against 1000. */
static void rail_rule(struct ach_slot* s, const struct ach_fold* f,
					  enum stats_column column, uint32_t now_ms) {
	/* GE_RIDE{minecart} is ORDERED, so this only ever sees ONE ride tick at a
	 * time, with that tick's own coordinates -- the v2 "latest coords of a
	 * fold" approximation is gone [design 5.2 v4]. */
	assert(f->count == 1u);
	if(!s->ride_origin_valid) {
		s->ride_origin_valid = true;
		s->ride_x = f->ride_x;
		s->ride_y = f->ride_y;
		s->ride_z = f->ride_z;
		return;
	}
	int64_t dx = (int64_t)s->ride_x - (int64_t)f->ride_x;
	int64_t dy = (int64_t)s->ride_y - (int64_t)f->ride_y;
	int64_t dz = (int64_t)s->ride_z - (int64_t)f->ride_z;
	if(dx * dx + dy * dy + dz * dz >= 1000)
		ach_award(s, 11, f->count, column, now_ms);
}

static const uint32_t s_move_stat[GE_MOVE_KIND_COUNT] = {
	2000u, /* walkOneCm  */
	2001u, /* swimOneCm  */
	2005u, /* diveOneCm  */
	2003u, /* climbOneCm */
	2004u, /* flyOneCm   */
};
static const uint32_t s_mount_stat[GE_MOUNT_KIND_COUNT] = {
	2006u, /* minecartOneCm */
	2007u, /* boatOneCm     */
	2008u, /* pigOneCm      */
};

/* Where a craftItem / useItem / breakItem count actually lands.
 *
 * StatList.<clinit> runs replaceAllSimilarBlocks() over ALL FOUR arrays, not
 * just mineBlock, so a block id that aliases shares one counter in the item
 * arrays too (R1 lead item 2) -- crafting grass counts under dirt exactly as
 * mining it does. It folds through stat_alias_block_in(), NOT
 * stat_canonical_block(): only the mine array is built behind Beta's
 * getEnableStats() gate, so a wooden door, sign, cake, repeater or bed -- all
 * disableStats() blocks -- still has an item-array counter. The ARRAY is a
 * parameter because replaceSimilarBlocks() picks its direction from which
 * side is registered in that array, and craftItem inverts two pairs (B3); see
 * the derivation in achievements_state.h. Item ids above the block range are
 * returned unchanged.
 *
 * CW_STAT_BLOCK_NONE means Beta has no counter for that block id in that
 * array at all (F3): an uncraftable block in craftItem, ANY block in
 * breakItem, and every id with no Beta block (0, 97..255 -- CaveWii's own
 * 97 and 200 included). Every caller must treat it as "apply nothing", which
 * also means the event is not `accepted`: nothing landed. */
static uint16_t item_counter_index(enum cw_stat_array array, uint16_t id) {
	return stat_alias_block_in(array, id);
}

/* A Packet 200 statistic for a non-achievement id [design 1.2 point 4]. */
static bool apply_remote_counter(struct ach_slot* s, const struct ach_fold* f,
								 enum stats_column column) {
	uint32_t id = f->id;
	int g = stat_general_index(id);
	if(g >= 0) {
		add_u64(&s->profile.general[g], column, f->amount);
		s->status.dirty = true;
		return true;
	}
	if(id >= CW_STAT_MINE_BASE
	   && id < CW_STAT_MINE_BASE + (uint32_t)CW_STAT_BLOCK_COUNT) {
		uint16_t b = stat_canonical_block((uint16_t)(id - CW_STAT_MINE_BASE));
		if(b == CW_STAT_BLOCK_NONE) /* filtered by stat_owner_of_id already */
			return false;
		add_u64(&s->profile.mine[b], column, f->amount);
		s->status.dirty = true;
		return true;
	}
	struct stats_pair* array = NULL;
	uint32_t base = 0;
	enum cw_stat_array which = CW_STAT_ARRAY_USE;
	if(id >= CW_STAT_CRAFT_BASE && id < CW_STAT_CRAFT_BASE + ITEMS_MAX) {
		array = s->profile.craft;
		base = CW_STAT_CRAFT_BASE;
		which = CW_STAT_ARRAY_CRAFT;
	} else if(id >= CW_STAT_USE_BASE && id < CW_STAT_USE_BASE + ITEMS_MAX) {
		array = s->profile.use;
		base = CW_STAT_USE_BASE;
		which = CW_STAT_ARRAY_USE;
	} else if(id >= CW_STAT_BREAK_BASE && id < CW_STAT_BREAK_BASE + ITEMS_MAX) {
		array = s->profile.brk;
		base = CW_STAT_BREAK_BASE;
		which = CW_STAT_ARRAY_BREAK;
	}
	if(!array)
		return false;
	/* The same alias fold a locally decided craft/use/break gets -- in the
	 * SAME array, so a remote craftItem folds the way craftItem folds -- and a
	 * remote server's craftItem[grass] and ours land on one counter. */
	{
		uint16_t slot = item_counter_index(which, (uint16_t)(id - base));
		if(slot == CW_STAT_BLOCK_NONE) /* filtered by stat_owner_of_id already */
			return false;
		add_u64(&array[slot], column, f->amount);
	}
	s->status.dirty = true;
	return true;
}

/* The 5.2 matrix, then the 1.1/1.2/1.4 rules. Pure arithmetic: no I/O, no
 * allocation, and it never emits, so it is safe to run under the lock. */
static void apply_fold(struct ach_slot* s, const struct ach_fold* f,
					   uint32_t now_ms) {
	int owner;
	if(f->kind == GE_REMOTE_STAT) {
		if(!stat_owner_of_id(f->id, &owner)) {
			s->status.rejected[ACH_REJECT_UNKNOWN_STAT] += f->count;
			return;
		}
	} else {
		owner = kind_owner((uint8_t)f->kind);
	}

	enum stats_column column = STATS_COLUMN_LOCAL;
	enum ach_reject_reason why = ACH_REJECT_BAD_EVENT;
	if(!matrix_accept((enum ach_bind_mode)s->status.mode, f->authority, owner,
					  &column, &why)) {
		s->status.rejected[why] += f->count;
		return;
	}

	/* `accepted` is counted AFTER the switch (R1 LOW 15). The matrix saying
	 * "this observer may report this class" is not the same as the event
	 * reaching a counter or an award: harvesting a block Beta gave no mine
	 * stat, picking up an item that triggers nothing, or a payload past the
	 * item registry all apply NOTHING, and counting them as accepted made the
	 * one number that says "this much progress landed" a lie. Every such path
	 * RETURNS out of the switch; every path that falls out of it has applied
	 * something (a counter, an award, or a ride-origin reset). */
	switch(f->kind) {
		/* -- server-decided facts ------------------------------------- */
		case GE_BLOCK_HARVESTED: {
			/* Block.harvestBlock -> mineBlock[canonical id]; a block Beta
			 * registered with disableStats() has no counter at all. */
			uint16_t b = stat_canonical_block((uint16_t)f->id);
			if(b == CW_STAT_BLOCK_NONE)
				return; /* nothing to count: leaves, bedrock, water, ... */
			add_u64(&s->profile.mine[b], column, f->count);
			s->status.dirty = true;
			break;
		}
		case GE_CRAFT_TAKEN: {
			/* ItemStack.onCrafting counts craftItem += stackSize whether or
			 * not the achievement fires; the parent gate below only ever
			 * drops the ACHIEVEMENT half [design 1.2 point 1]. */
			int a = craft_achievement_for(f->id);
			/* Beta registers a craftItem counter only for a recipe/smelting
			 * output, so a craft of an uncraftable block id reaches NO
			 * counter (F3) exactly as an id past the registry does. Both of
			 * the craft achievements that name a BLOCK -- workbench 58,
			 * furnace 61 -- are outputs, so the award half is never the thing
			 * being dropped here; it is still checked first. */
			uint16_t slot = (f->id < ITEMS_MAX)
				? item_counter_index(CW_STAT_ARRAY_CRAFT, (uint16_t)f->id)
				: CW_STAT_BLOCK_NONE;
			if(slot == CW_STAT_BLOCK_NONE && a < 0)
				return; /* no counter and no achievement: nothing applied */
			if(slot != CW_STAT_BLOCK_NONE) {
				add_u64(&s->profile.craft[slot], column, f->amount);
				s->status.dirty = true;
			}
			ach_award(s, a, f->count, column, now_ms);
			break;
		}
		case GE_FURNACE_TAKEN: {
			/* SlotFurnace's take is a craftItem count too, under the same
			 * registration gate: iron (265) and cooked fish (350) are item
			 * ids and always counted, an ORE id taken out of a furnace is not
			 * a smelting output and reaches no counter. */
			uint16_t slot = (f->id < ITEMS_MAX)
				? item_counter_index(CW_STAT_ARRAY_CRAFT, (uint16_t)f->id)
				: CW_STAT_BLOCK_NONE;
			if(slot == CW_STAT_BLOCK_NONE && f->id != 265 && f->id != 350)
				return;
			if(slot != CW_STAT_BLOCK_NONE) {
				add_u64(&s->profile.craft[slot], column, f->amount);
				s->status.dirty = true;
			}
			/* SlotFurnace checks iron and cooked fish INDEPENDENTLY (two
			 * ifs, not an else-if chain) [design 1.1]. */
			if(f->id == 265)
				ach_award(s, 5, f->count, column, now_ms); /* acquireIron */
			if(f->id == 350)
				ach_award(s, 10, f->count, column, now_ms); /* cookFish */
			break;
		}
		case GE_ITEM_PICKED_UP:
			/* EntityItem.onCollideWithPlayer: the PICKUP is the trigger, not
			 * the break or the kill. Beta has no "items picked up" stat, so
			 * there is no counter here [design 1.4] -- a pickup of anything
			 * else, or one whose parent is locked, applies nothing at all. */
			if(f->id == 17) {
				if(!ach_award(s, 1, f->count, column, now_ms)) /* mineWood */
					return;
			} else if(f->id == 334) {
				if(!ach_award(s, 14, f->count, column, now_ms)) /* killCow */
					return;
			} else {
				return;
			}
			break;
		case GE_ITEM_USED: {
			uint16_t slot;
			if(f->id >= ITEMS_MAX)
				return;
			slot = item_counter_index(CW_STAT_ARRAY_USE, (uint16_t)f->id);
			if(slot == CW_STAT_BLOCK_NONE)
				return; /* no Beta block at that id: air, or a CaveWii-only one */
			add_u64(&s->profile.use[slot], column, f->count);
			s->status.dirty = true;
			break;
		}
		case GE_ITEM_BROKEN: {
			uint16_t slot;
			if(f->id >= ITEMS_MAX)
				return;
			slot = item_counter_index(CW_STAT_ARRAY_BREAK, (uint16_t)f->id);
			/* Beta's breakItem is `isDamagable()` only, and no block item is:
			 * a BLOCK id can never reach a breakItem counter (F3). */
			if(slot == CW_STAT_BLOCK_NONE)
				return;
			add_u64(&s->profile.brk[slot], column, f->count);
			s->status.dirty = true;
			break;
		}
		case GE_KILLED:
			add_general(s, 2023u, column, f->count); /* mobKills */
			if(mob_is_hostile_for_kill_enemy(f->sub))
				ach_award(s, 13, f->count, column, now_ms); /* killEnemy */
			break;
		case GE_KILLED_PLAYER:
			add_general(s, 2024u, column, f->count); /* playerKills */
			break;
		case GE_DAMAGE_DEALT: add_general(s, 2020u, column, f->amount); break;
		case GE_DAMAGE_TAKEN: add_general(s, 2021u, column, f->amount); break;
		case GE_DIED: add_general(s, 2022u, column, f->count); break;
		case GE_FISH_CAUGHT: add_general(s, 2025u, column, f->count); break;
		case GE_PIG_FELL_WITH_RIDER:
			/* EntityPig.fall: `f > 5F` with a player rider [design 1.1 row 15,
			 * EntityPig.java:112] -- ON THE FLOAT, which is why the fold
			 * carries `fall` and not a centimetre count (R2 MED 5). The kind
			 * is ORDERED, so this is always ONE fall with its own distance:
			 * Beta's one addStat per qualifying fall falls straight out. */
			assert(f->count == 1u);
			if(!pig_fall_qualifies(f->fall)
			   || !ach_award(s, 15, f->count, column, now_ms))
				return;
			break;

		/* -- client-decided facts ------------------------------------- */
		case GE_JUMPED: add_general(s, 2010u, column, f->count); break;
		case GE_DROPPED: add_general(s, 2011u, column, f->count); break;
		case GE_INVENTORY_OPENED:
			if(!ach_award(s, 0, f->count, column, now_ms)) /* openInventory */
				return;
			break;
		case GE_MOVED:
			/* A pure distance counter and nothing else. It used to reset the
			 * ride origin here, on the reasoning that addMovementStat returns
			 * early while riding so a plain move means the ride ended -- true,
			 * but NOT where Beta resets (R2 MED 4): EntityPlayer.onUpdate does
			 * it on every non-riding tick, whether or not the player moved.
			 * That reset now lives on GE_TICK, which is emitted
			 * unconditionally, so a player who stands still after dismounting
			 * still gets it -- and GE_MOVED goes back to FOLDABLE. */
			if(f->sub < GE_MOVE_KIND_COUNT)
				add_general(s, s_move_stat[f->sub], column, f->amount);
			break;
		case GE_FELL: add_general(s, 2002u, column, f->amount); break;
		case GE_RIDE:
			if(f->sub < GE_MOUNT_KIND_COUNT)
				add_general(s, s_mount_stat[f->sub], column, f->amount);
			if(f->sub == GE_MOUNT_MINECART)
				rail_rule(s, f, column, now_ms);
			break;
		case GE_RIDE_END: ride_origin_reset(s); break;
		case GE_GAME_STARTED:
			add_general(s, 1000u, column, f->count); /* startGame */
			add_general(s, (f->sub == GE_WORLD_NEW) ? 1001u : 1002u, column,
						f->count); /* createWorld / loadWorld */
			break;
		case GE_GAME_LEFT: add_general(s, 1004u, column, f->count); break;
		case GE_MP_JOINED: add_general(s, 1003u, column, f->count); break;
		case GE_TICK:
			/* THE FAITHFUL RAIL-ORIGIN RESET [EntityPlayer.java:123-127]:
			 *
			 *	   addStat(StatList.minutesPlayedStat, 1);
			 *	   if(ridingEntity == null)
			 *	   {
			 *		   startMinecartRidingCoordinate = null;
			 *	   }
			 *
			 * Beta clears the origin on EVERY tick the player is not riding,
			 * in onUpdate, right after the play-time stat -- not in
			 * addMovementStat. `sub` carries which mount (0 = none), so a lost
			 * GE_RIDE_END can no longer leave a stale origin for the next ride
			 * to cross the onARail threshold against (R2 MED 4). The two
			 * statements are in Beta's own order (count, then reset); they
			 * share no state, so the order is cosmetic and kept anyway. */
			add_general(s, 1100u, column, f->count); /* playOneMinute */
			if(f->sub == (uint8_t)GE_TICK_NOT_RIDING)
				ride_origin_reset(s);
			/* EntityPlayerSP.onLivingUpdate shows the openInventory hint
			 * every tick while it is locked [design 1.1 row 0]. It is a FLAG,
			 * not a queued toast (R1 LOW 11): queueing it made notices_queued
			 * a tick counter and reset the notice's start stamp 20 times a
			 * second. Nothing else can unlock first -- every other achievement
			 * descends from it -- so the flag is exactly "openInventory is
			 * still locked". */
			hint_refresh(s);
			break;

		/* -- a remote Beta server's Packet 200 ------------------------ */
		case GE_REMOTE_STAT: {
			int a = ach_index_of_stat_id(f->id);
			if(a >= 0) {
				/* func_27027_b funnels into the same addStat path, so the
				 * parent check and the toast apply; the increment is the
				 * packet's amount, not one per packet [design 1.2 point 4]. */
				if(!ach_award(s, a, f->amount, column, now_ms))
					return;
				break;
			}
			if(!apply_remote_counter(s, f, column))
				return;
			break;
		}
		default: break;
	}
	s->status.accepted += f->count;
}

/* Is this actor allowed to affect `slot` right now? */
static bool actor_ok(const struct ach_slot* s, uint64_t generation,
					 enum ach_reject_reason* why) {
	if(!s->status.bound) {
		*why = ACH_REJECT_UNBOUND;
		return false;
	}
	if(s->status.generation != generation) {
		*why = ACH_REJECT_STALE_GENERATION;
		return false;
	}
	return true;
}

static void fold_from_event(struct ach_fold* f, const struct game_event* ev) {
	memset(f, 0, sizeof(*f));
	f->kind = ev->kind;
	f->sub = ev->sub;
	f->authority = ev->authority;
	f->id = fold_key_id(ev);
	f->used = 1;
	f->count = 1;
	f->amount = payload_amount(ev);
	/* The ring path is the EXACT one: this event's own coordinates and its own
	 * fall distance, so the rail rule and the pig rule see every event as it
	 * happened [design 5.2 v4]. */
	if(ev->kind == GE_RIDE) {
		f->ride_x = ev->u.ride.x;
		f->ride_y = ev->u.ride.y;
		f->ride_z = ev->u.ride.z;
	}
	if(ev->kind == GE_PIG_FELL_WITH_RIDER)
		f->fall = ev->u.fall.f;
}

/* One ring entry: the actor gate, then the exact single-event apply. */
static void drain_ring_event(const struct game_event* ev, uint32_t now_ms) {
	struct ach_slot* s = &s_slot[ev->who.slot];
	enum ach_reject_reason why;
	if(!actor_ok(s, ev->who.generation, &why)) {
		s->status.rejected[why]++;
		return;
	}
	struct ach_fold f;
	fold_from_event(&f, ev);
	apply_fold(s, &f, now_ms);
}

/* Caller holds s_lock.
 *
 * DRAIN ORDER [design 5.2 v4]: the main ring, then the ordered overflow ring,
 * then each slot's coalescer.
 *
 * The first two are the ordered path and they are drained in that sequence for
 * one reason: the main ring only ever FILLS during a stalled window (it is
 * never drained mid-burst), so for any one slot every event in the overflow
 * ring was emitted after every event still in the main ring. Concatenating the
 * two FIFOs therefore reproduces emit order exactly, whatever thread emitted
 * what and wherever the split landed.
 *
 * The coalescer goes last and in plain insertion order. It holds FOLDABLE
 * kinds only -- pure counters -- so no achievement and no notice can depend on
 * where it lands, which is what retired the v2 rank-ordered drain. */
static void drain_coalescers_locked(uint32_t now_ms);

static void drain_locked(uint32_t now_ms) {
	while(s_ring_count > 0) {
		struct game_event ev = s_ring[s_ring_head];
		if(lifecycle_event(&ev)) break;
		s_ring_head = (s_ring_head + 1u) % CW_ACH_RING_CAPACITY;
		s_ring_count--;
		drain_ring_event(&ev, now_ms);
	}

	while(!s_ring_count && s_ovf_count > 0) {
		struct game_event ev = s_ovf[s_ovf_head];
		if(lifecycle_event(&ev)) break;
		s_ovf_head = (s_ovf_head + 1u) % CW_ACH_OVERFLOW_CAPACITY;
		s_ovf_count--;
		struct ach_status* st = &s_slot[ev.who.slot].status;
		if(st->overflow_used > 0)
			st->overflow_used--;
		drain_ring_event(&ev, now_ms);
	}

	drain_coalescers_locked(now_ms);
}

static void drain_coalescers_locked(uint32_t now_ms) {
	for(int i = 0; i < CW_ACH_SLOT_COUNT; i++) {
		struct ach_slot* s = &s_slot[i];
		if(s->co.overflow && s_route[i].managed) s_route[i].incomplete = true;
		if((s_route[i].incomplete && stats_profile_id_valid(s->status.profile_id)
			&& !s->profile.progress_incomplete)
		   || (s->co.overflow && !s_route[i].managed)) {
			/* Same rule as the emit path: the flag is only useful persisted,
			 * so whoever sets it marks the slot dirty (R2 MED 7). */
			s->profile.progress_incomplete = 1;
			s->status.progress_incomplete = true;
			s->status.dirty = true;
		}
		if(!s->co.used) { s->co.overflow = false; continue; }
		for(uint16_t e = 0; e < s->co.used; e++) {
			struct ach_fold* f = &s->co.e[e];
			if(!f->used)
				continue;
			/* A BEGIN in the FIFO owns a future epoch. Retain its folded
			 * counters until that profile has actually loaded on main. */
			if(s_route[i].managed && (!s->status.bound
			   || f->generation != s->status.generation)) continue;
			/* A coalescer entry is a counter sum and nothing else: it can
			 * never carry ride coordinates or a pig-fall distance, because
			 * GE_RIDE{minecart} and GE_PIG_FELL_WITH_RIDER are ORDERED and
			 * never fold. */
			assert(f->ride_x == 0 && f->ride_y == 0 && f->ride_z == 0);
			assert(f->fall == 0.0F);
			enum ach_reject_reason why;
			if(!actor_ok(s, s->status.generation, &why)) {
				s->status.rejected[why] += f->count;
				f->used = false;
				continue;
			}
			apply_fold(s, f, now_ms);
			f->used = false;
		}
		/* Compact/reindex retained epochs, without allocation or another
		 * state block. This also returns old-owner key capacity before rebind. */
		uint16_t retained = 0;
		s_coalescer_rebuilds++;
		for(uint16_t e = 0; e < s->co.used; e++)
			if(s->co.e[e].used) s->co.e[retained++] = s->co.e[e];
		coalescer_clear(&s->co);
		s->co.used = retained;
		for(uint16_t e = 0; e < retained; e++) {
			struct ach_fold* f = &s->co.e[e];
			uint32_t bucket = (fold_hash(f->kind, f->sub, f->authority, f->id)
				^ (uint32_t)f->generation ^ (uint32_t)(f->generation >> 32))
				& (CW_ACH_COALESCE_BUCKETS - 1u);
			while(s->co.bucket[bucket] != CW_ACH_BUCKET_EMPTY)
				bucket = (bucket + 1u) & (CW_ACH_COALESCE_BUCKETS - 1u);
			s->co.bucket[bucket] = (int16_t)e;
		}
		s->status.coalesce_used = retained;
	}
}

/* ------------------------------------------------------ 7. the lifecycle */

/* `<data root>/stats` must exist AND be a directory.
 *
 * EEXIST alone is not success (R1 LOW 13): a REGULAR FILE at that path also
 * returns EEXIST, and accepting it left the module "initialised" against a
 * path into which every single save then failed with ERR_TMP_OPEN, forever,
 * with nothing saying why. stat() is what distinguishes the two. */
static bool make_stats_dir(const char* dir) {
#ifdef _WIN32
	int rc = _mkdir(dir);
#else
	int rc = mkdir(dir, 0755);
#endif
	if(rc == 0)
		return true;
	if(errno != EEXIST)
		return false;
	struct stat st;
	if(stat(dir, &st) != 0)
		return false;
#ifdef _WIN32
	return (st.st_mode & S_IFMT) == S_IFDIR;
#else
	return S_ISDIR(st.st_mode);
#endif
}

void achievements_set_allocator(void* (*alloc_fn)(size_t),
								void (*free_fn)(void*)) {
	s_alloc = alloc_fn;
	s_free = free_fn;
}

size_t achievements_block_bytes(void) {
	return sizeof(struct ach_block);
}

enum ach_init_status achievements_init_status(void) {
	return (enum ach_init_status)s_init_status;
}

static void block_release(void) {
	if(!s_blk)
		return;
	if(s_free)
		s_free(s_blk);
	else
		mem2_free(s_blk);
	s_blk = NULL;
}

/* Fail every init from here on, with a reason. */
static bool init_failed(enum ach_init_status why) {
	s_init_status = (uint8_t)why;
	s_stats_dir[0] = '\0';
	return false;
}

static bool quiesce_flush_all(void);

/* Freeze the global stats path before any world-cwd switch. Do not collapse
 * POSIX/FAT '..' components: leave their native filesystem meaning intact. */
static bool stats_root_path(char* out, size_t cap, const char* root) {
	if(!root || !root[0])
		return false;
	char absolute[CW_STATS_PATH_MAX];
#ifdef _WIN32
	if(!_fullpath(absolute, root, sizeof(absolute)))
		return false;
#else
	int n;
#ifdef PLATFORM_WII
	const char* colon = strchr(root, ':');
	if(colon && colon[1] == '/')
		n = snprintf(absolute, sizeof(absolute), "%s", root);
#else
	if(root[0] == '/')
		n = snprintf(absolute, sizeof(absolute), "%s", root);
#endif
	else {
		char cwd[CW_STATS_PATH_MAX];
		if(!getcwd(cwd, sizeof(cwd)))
			return false;
#ifdef PLATFORM_WII
		/* A slash-only Wii path is relative to the current FAT device. */
		const char* device = strchr(cwd, ':');
		if(root[0] == '/' && device)
			n = snprintf(absolute, sizeof(absolute), "%.*s:%s",
				(int)(device - cwd), cwd, root);
		else if(colon)
			return false; /* ambiguous device-relative root */
		else
#endif
			n = snprintf(absolute, sizeof(absolute), "%s/%s", cwd, root);
	}
	if(n < 0 || (size_t)n >= sizeof(absolute))
		return false;
#endif
	int written = snprintf(out, cap, "%s/stats", absolute);
	char longest[CW_STATS_PATH_MAX];
	return written > 0 && (size_t)written < cap
		&& stats_profile_path(longest, sizeof(longest), out,
			"1234567890123456", ".bad");
}

bool achievements_init(const char* data_root) {
	if(!s_lock_ready) {
		if(!thread_mutex_init(&s_lock))
			return init_failed(ACH_INIT_NO_MUTEX);
		s_lock_ready = true;
	}
	/* Re-init wipes every slot, so anything applied but not yet written would
	 * be lost silently. Flush first, with the OLD data root still in place --
	 * a re-init is never a reason to drop progress. */
	if(__atomic_load_n(&s_inited, __ATOMIC_ACQUIRE)
	   && !quiesce_flush_all()) {
		s_init_status = (uint8_t)ACH_INIT_SAVE_PENDING;
		return false;
	}
	__atomic_store_n(&s_inited, false, __ATOMIC_RELEASE);

	thread_mutex_lock(&s_lock);
	if(!s_blk) {
		/* The module's whole mutable state, in ONE MEM2 block -- never MEM1
		 * .bss. See struct ach_block. */
		s_blk = s_alloc ? s_alloc(sizeof(struct ach_block))
						: mem2_alloc(sizeof(struct ach_block));
		if(s_blk)
			memset(s_blk, 0, sizeof(*s_blk));
	}
	if(!s_blk) {
		thread_mutex_unlock(&s_lock);
		/* Disabled, loudly: every emit is dropped and counted, every query is
		 * empty, and the status says which failure it was. */
		return init_failed(ACH_INIT_NO_MEMORY);
	}
	s_ring_head = 0;
	s_ring_count = 0;
	s_ovf_head = 0;
	s_ovf_count = 0;
	memset(s_route, 0, sizeof(s_route));
	s_control_overflow = 0;
	memset(s_save_requested, 0, sizeof(s_save_requested));
	memset(s_save_error_ack, 0, sizeof(s_save_error_ack));
	memset(s_save_serial, 0, sizeof(s_save_serial));
	memset(s_blk->save, 0, sizeof(s_blk->save));
	s_profiles_valid = true;
	snprintf(s_profiles[0], sizeof(s_profiles[0]), "player1");
	snprintf(s_profiles[1], sizeof(s_profiles[1]), "player2");
	for(int i = 0; i < CW_ACH_SLOT_COUNT; i++) {
		struct ach_slot* s = &s_slot[i];
		memset(&s->status, 0, sizeof(s->status));
		stats_profile_reset(&s->profile, NULL);
		s->notice.ach_id = -1;
		s->notice.started_ms = 0;
		s->notice.hint_active = false;
		s->notice_head = s->notice_count = 0;
		ride_origin_reset(s);
		coalescer_clear(&s->co);
	}
	s_stats_dir[0] = '\0';
	thread_mutex_unlock(&s_lock);

	__atomic_store_n(&s_bad_slot_drops, 0u, __ATOMIC_RELAXED);
	__atomic_store_n(&s_pre_init_drops, 0u, __ATOMIC_RELAXED);
	s_epoch = time_get();

	/* `<data root>/stats` [design 4]. This is the ONE place that creates the
	 * directory: achievements_persist.c deliberately never does, so a save
	 * into a missing directory fails loudly instead of landing somewhere
	 * else. */
	if(!stats_root_path(s_stats_dir, sizeof(s_stats_dir), data_root))
		return init_failed(ACH_INIT_BAD_ARG);
	if(!make_stats_dir(s_stats_dir))
		return init_failed(ACH_INIT_BAD_STATS_DIR);
	s_init_status = (uint8_t)ACH_INIT_OK;
	__atomic_store_n(&s_inited, true, __ATOMIC_RELEASE);
	return true;
}

bool achievements_shutdown(void) {
	if(!s_lock_ready)
		return true;
	/* Same contract as a re-init: never drop applied progress. */
	if(__atomic_load_n(&s_inited, __ATOMIC_ACQUIRE)
	   && !quiesce_flush_all())
		return false;
	__atomic_store_n(&s_inited, false, __ATOMIC_RELEASE);
	/* Under the lock, so an emit that read s_inited just before this cannot
	 * reach a freed block: it blocks here, then finds s_blk NULL. */
	thread_mutex_lock(&s_lock);
	block_release();
	s_ring_head = 0;
	s_ring_count = 0;
	s_ovf_head = 0;
	s_ovf_count = 0;
	s_stats_dir[0] = '\0';
	thread_mutex_unlock(&s_lock);
	s_init_status = (uint8_t)ACH_INIT_NEVER_RUN;
	return true;
}

void achievements_set_clock(uint32_t (*now)(void)) {
	stats_set_clock(now);
}

enum ach_bind_status achievements_bind(uint8_t slot, uint64_t generation,
									   const char* profile_id,
									   enum ach_bind_mode mode) {
	/* The key rule lives in B1 (stats_profile_id_valid) so the bind path and
	 * both file paths can never disagree about what a key is (R1 MED 3). */
	if(slot >= CW_ACH_SLOT_COUNT || !stats_profile_id_valid(profile_id)
	   || (mode != ACH_MODE_LOCAL_WORLD && mode != ACH_MODE_REMOTE_SERVER)
	   || !__atomic_load_n(&s_inited, __ATOMIC_ACQUIRE))
		return ACH_BIND_BAD_KEY;

	thread_mutex_lock(&s_lock);
	for(int i = 0; i < CW_ACH_SLOT_COUNT; i++) {
		if(i == (int)slot)
			continue;
		if((s_slot[i].status.bound || s_slot[i].status.dirty)
		   && strcmp(s_slot[i].status.profile_id, profile_id) == 0) {
			thread_mutex_unlock(&s_lock);
			return ACH_BIND_DUPLICATE; /* two players, never one file */
		}
	}
	if(s_slot[slot].status.dirty
	   && (!s_slot[slot].status.bound || s_slot[slot].status.closing)) {
		thread_mutex_unlock(&s_lock);
		return ACH_BIND_SAVE_PENDING;
	}
	if(s_slot[slot].status.bound) {
		thread_mutex_unlock(&s_lock);
		return ACH_BIND_BUSY; /* unbind has to run first */
	}
	thread_mutex_unlock(&s_lock);

	/* The load touches the filesystem, so it runs outside the lock; the slot
	 * is still unbound here, so no drain can be applying to it. */
	struct ach_slot* s = &s_slot[slot];
	enum stats_load_status load = STATS_LOAD_BAD_ARG;
	stats_profile_load(s_stats_dir, profile_id, &s->profile, &load);

	thread_mutex_lock(&s_lock);
	memset(&s->status, 0, sizeof(s->status));
	s->status.bound = true;
	s->status.mode = (uint8_t)mode;
	s->status.generation = generation;
	snprintf(s->status.profile_id, sizeof(s->status.profile_id), "%s",
			 profile_id);
	s->status.load = (uint8_t)load;
	s->status.progress_incomplete = s->profile.progress_incomplete != 0;
	s->status.init_status = s_init_status;
	s->notice.ach_id = -1;
	s->notice.started_ms = 0;
	s->notice_head = s->notice_count = 0;
	/* The hint is a property of the LOADED profile, not of a tick: a player
	 * who has never opened their inventory sees it from the first frame, and a
	 * profile that already has openInventory never does [design 7]. */
	hint_refresh(s);
	ride_origin_reset(s);
	if(!s_route[slot].managed) coalescer_clear(&s->co);
	thread_mutex_unlock(&s_lock);
	return ACH_BIND_OK;
}

/* Runtime-armed test seam; see achievements.h. Fires under s_lock, inside the
 * status update, so a test can prove the update is atomic against a snapshot. */
static void (*s_status_barrier)(void);

void achievements_debug_status_barrier(void (*fn)(void)) {
	s_status_barrier = fn;
}

void achievements_debug_notice_hook(void (*fn)(uint8_t slot, int16_t ach_id)) {
	s_notice_hook = fn;
}

/* Write one slot's file if it has anything new.
 *
 * TWO REGIONS, one rule each (R1 MED 7): the file I/O runs with the lock
 * RELEASED -- a save is milliseconds of libfat on the Wii and nothing may
 * block a producer's emit for that long -- while every read and every write of
 * `status` happens with it HELD. The old version did the I/O outside the lock
 * but also wrote save_failures and last_save_error outside it, so a concurrent
 * achievements_status() (which copies the whole struct under the lock) could
 * return a snapshot with the counter bumped and the reason not yet stored. */

static void save_result(struct ach_slot* s, enum stats_save_status st,
	const struct stats_profile* snapshot) {
	thread_mutex_lock(&s_lock);
	unsigned i = s - s_slot;
	if(st == STATS_SAVE_OK) {
		s_save_serial[i]++;
		s->status.dirty = snapshot && memcmp(snapshot, &s->profile, sizeof(*snapshot)) != 0;
		s->profile.main_was_corrupt = 0;
		s->status.last_save_error = STATS_SAVE_OK;
		s_route[i].blocked = false;
	} else {
		s->status.save_failures++;
		if(s_status_barrier) { void (*fn)(void) = s_status_barrier; s_status_barrier = NULL; fn(); }
		s->status.last_save_error = st;
		s_route[i].blocked = true;
	}
	thread_mutex_unlock(&s_lock);
}

static void save_step(unsigned i) {
	struct ach_save* save = &s_blk->save[i];
	if(!save->active || !stats_save_step(&save->job)) return;
	save->active = false;
	if(save->job.rescue) {
		fprintf(stderr, "[achievements] background survivor profile=%s result=%u\n", save->snapshot.profile_id, save->job.result);
		return; /* A survivor never certifies the canonical profile as durable. */
	}
	save_result(&s_slot[i], save->job.result, &save->snapshot);
}

static void flush_slot(struct ach_slot* s) {
	unsigned i = s - s_slot;
	struct ach_save* save = &s_blk->save[i];
	if(s_in_tick && s_tick_save_attempted[i]) return;
	if(save->active) {
		if(s_cooperative_saves) return;
		while(save->active) save_step(i);
		if(save->job.result != STATS_SAVE_OK) return;
		/* Success may leave later applied facts dirty. Explicit off-frame flush
			* commits those too; a failed attempt is never silently retried. */
	}
	thread_mutex_lock(&s_lock);
	bool go = s->status.dirty;
	if(go && s_in_tick) s_tick_save_attempted[i] = true;
	if(go && s_cooperative_saves) {
		save->snapshot = s->profile;
		stats_save_begin(&save->job, s_stats_dir, &save->snapshot);
		save->active = true;
	}
	thread_mutex_unlock(&s_lock);
	if(go && !s_cooperative_saves) save_result(s, stats_profile_save(s_stats_dir, &s->profile), NULL);
}

void achievements_set_cooperative_saves(bool enabled) { s_cooperative_saves = enabled; }
bool achievements_save_busy(uint8_t slot) {
	return s_blk && slot < CW_ACH_SLOT_COUNT && s_blk->save[slot].active && !s_blk->save[slot].job.rescue;
}
uint32_t achievements_save_serial(uint8_t slot) {
	return slot < CW_ACH_SLOT_COUNT ? s_save_serial[slot] : 0;
}
bool achievements_slot_save_failed(uint8_t slot) {
	if(!s_blk || slot >= CW_ACH_SLOT_COUNT) return false;
	thread_mutex_lock(&s_lock);
	bool failed = s_route[slot].blocked && !achievements_save_busy(slot) && !s_save_requested[slot];
	thread_mutex_unlock(&s_lock); return failed;
}
bool achievements_save_error_acknowledged(uint8_t slot) {
 return slot < CW_ACH_SLOT_COUNT && s_save_error_ack[slot];
}
void achievements_acknowledge_save_error(uint8_t slot) {
 if(slot < CW_ACH_SLOT_COUNT) s_save_error_ack[slot] = true;
}
bool achievements_save_durable(uint8_t slot) {
	if(!s_blk || slot >= CW_ACH_SLOT_COUNT) return true;
	thread_mutex_lock(&s_lock);
	bool durable = !s_slot[slot].status.dirty && !s_blk->save[slot].active;
	thread_mutex_unlock(&s_lock); return durable;
}
void achievements_request_save(uint8_t slot) {
	if(!s_blk || slot >= CW_ACH_SLOT_COUNT) return;
	s_save_error_ack[slot] = false;
	s_save_requested[slot] = true;
}
void achievements_request_rescue(uint8_t slot) {
	if(!s_blk || slot >= CW_ACH_SLOT_COUNT || !s_slot[slot].status.dirty) return;
	s_save_error_ack[slot] = true;
	struct ach_save* save = &s_blk->save[slot];
	if(save->active && save->job.rescue) return;
	if(save->active) stats_save_cancel(&save->job);
	thread_mutex_lock(&s_lock);
	save->snapshot = s_slot[slot].profile;
	stats_rescue_begin(&save->job, s_stats_dir, &save->snapshot);
	save->active = true;
	s_save_requested[slot] = false;
	thread_mutex_unlock(&s_lock);
}
bool achievements_rescue_unsaved(uint8_t slot) {
	if(!s_blk || slot >= CW_ACH_SLOT_COUNT || !s_slot[slot].status.dirty) return true;
	if(s_blk->save[slot].active) {
		stats_save_cancel(&s_blk->save[slot].job); s_blk->save[slot].active = false;
	}
	thread_mutex_lock(&s_lock);
	s_blk->save[slot].snapshot = s_slot[slot].profile;
	thread_mutex_unlock(&s_lock);
	enum stats_save_status st = stats_profile_rescue(s_stats_dir, &s_blk->save[slot].snapshot);
	fprintf(stderr, "[achievements] explicit quit without saving profile=%s unsaved-survivor=%s error=%u\n",
		s_slot[slot].status.profile_id, st == STATS_SAVE_OK ? "written" : "unavailable", st);
	return st == STATS_SAVE_OK;
}


/* Profile keys are fixed before providers start. Re-entering the same owner
	* can keep its unsaved resident counters instead of waiting for its file.
	* Public arbitrary-key bind still refuses replacement of dirty progress. */
static bool begin_owner(const struct game_event* ev) {
	unsigned i = ev->who.slot;
	s_save_error_ack[i] = false;
	thread_mutex_lock(&s_lock);
	struct ach_slot* s = &s_slot[i];
	bool retained = !s->status.bound && (s->status.dirty || s_blk->save[i].active)
		&& strcmp(s->status.profile_id, s_profiles[i]) == 0;
	if(retained) {
		s->status.bound = true; s->status.closing = false;
		s->status.generation = ev->who.generation; s->status.mode = ev->sub;
		ride_origin_reset(s); hint_refresh(s);
	}
	thread_mutex_unlock(&s_lock);
	return retained || achievements_bind(i, ev->who.generation, s_profiles[i], ev->sub) == ACH_BIND_OK;
}

static void drain_lifecycle_ring(bool overflow, uint32_t now_ms) {
	thread_mutex_lock(&s_lock);
	unsigned count = overflow ? s_ovf_count : s_ring_count;
	thread_mutex_unlock(&s_lock);
	for(unsigned n = 0; n < count; n++) {
		thread_mutex_lock(&s_lock);
		if(!(overflow ? s_ovf_count : s_ring_count)) { thread_mutex_unlock(&s_lock); break; }
		struct game_event ev = overflow ? s_ovf[s_ovf_head] : s_ring[s_ring_head];
		unsigned i = ev.who.slot;
		struct ach_slot* s = &s_slot[i];
		bool match = s->status.bound && s->status.generation == ev.who.generation;
		if(overflow) { s_ovf_head = (s_ovf_head + 1) % CW_ACH_OVERFLOW_CAPACITY; s_ovf_count--; }
		else { s_ring_head = (s_ring_head + 1) % CW_ACH_RING_CAPACITY; s_ring_count--; }
		if(!lifecycle_event(&ev)) {
			drain_ring_event(&ev, now_ms);
			if(overflow && s->status.overflow_used) s->status.overflow_used--;
			thread_mutex_unlock(&s_lock); continue;
		}
		drain_coalescers_locked(now_ms);
		thread_mutex_unlock(&s_lock);
		if(ev.kind == GE_SESSION_BEGIN) {
			if(!begin_owner(&ev)) {
				thread_mutex_lock(&s_lock);
				s_route[i].blocked = s_route[i].incomplete = true;
				s_control_overflow |= 1u << i;
				thread_mutex_unlock(&s_lock);
			}
		} else if(match) {
			bool already_saving = s_blk->save[i].active;
			flush_slot(s);
			if(already_saving) s_save_requested[i] = true;
			thread_mutex_lock(&s_lock);
			if(ev.kind == GE_SESSION_END) {
				s->status.bound = s->status.closing = false;
				ride_origin_reset(s); s->notice_head = s->notice_count = 0;
				s->notice = (struct ach_notice){.ach_id = -1};
			}
			thread_mutex_unlock(&s_lock);
		}
		thread_mutex_lock(&s_lock);
		if(ev.kind == GE_SESSION_CHECKPOINT && s_route[i].checkpoint == ev.who.generation)
			s_route[i].checkpoint = 0;
		thread_mutex_unlock(&s_lock);
	}
}

void achievements_tick(void) {
	if(!__atomic_load_n(&s_inited, __ATOMIC_ACQUIRE)) return;
	s_in_tick = true;
	memset(s_tick_save_attempted, 0, sizeof(s_tick_save_attempted));
	uint32_t now_ms = module_now_ms();
	for(unsigned i = 0; i < CW_ACH_SLOT_COUNT; i++) {
		thread_mutex_lock(&s_lock);
		bool retry = s_route[i].retry && !s_route[i].checkpoint;
		s_route[i].retry = false;
		thread_mutex_unlock(&s_lock);
		if(retry && s_route[i].blocked) flush_slot(&s_slot[i]);
	}
	drain_lifecycle_ring(false, now_ms);
	/* Main-ring entries always precede the overflow ring. */
	drain_lifecycle_ring(true, now_ms);
	thread_mutex_lock(&s_lock); drain_coalescers_locked(now_ms); thread_mutex_unlock(&s_lock);
	for(unsigned i = 0; i < CW_ACH_SLOT_COUNT; i++) {
		thread_mutex_lock(&s_lock);
		bool recover = (s_control_overflow & (1u << i)) != 0;
		uint64_t epoch = s_route[i].epoch;
		uint8_t mode = s_route[i].mode;
		thread_mutex_unlock(&s_lock);
		if(recover) {
			/* Overflow is a lost boundary, not an excuse to stop tracking this owner
				* until quit. Reconcile to the latest published admission during play. */
			thread_mutex_lock(&s_lock);
			if(s_slot[i].status.generation != epoch || !epoch) {
				s_slot[i].status.bound = false;
				ride_origin_reset(&s_slot[i]);
				s_slot[i].notice_head = s_slot[i].notice_count = 0;
				s_slot[i].notice = (struct ach_notice){.ach_id = -1};
			}
			thread_mutex_unlock(&s_lock);
			bool recovered = !epoch || s_slot[i].status.bound
				|| begin_owner(&(struct game_event){.who = {.slot = i, .generation = epoch}, .sub = mode});
			if(recovered) {
				thread_mutex_lock(&s_lock);
				if(stats_profile_id_valid(s_slot[i].status.profile_id)) {
					s_slot[i].profile.progress_incomplete = 1;
					s_slot[i].status.progress_incomplete = s_slot[i].status.dirty = true;
				}
				drain_coalescers_locked(now_ms);
				s_control_overflow &= ~(1u << i);
				thread_mutex_unlock(&s_lock);
				flush_slot(&s_slot[i]);
			}
		}
		if(s_save_requested[i] && !s_blk->save[i].active) {
			s_save_requested[i] = false; flush_slot(&s_slot[i]);
		}
		save_step(i);
	}
	s_in_tick = false;
}

bool achievements_lifecycle_retry(void) {
	if(!s_blk || !__atomic_load_n(&s_inited, __ATOMIC_ACQUIRE)) return false;
	thread_mutex_lock(&s_lock);
	for(unsigned i = 0; i < CW_ACH_SLOT_COUNT; i++) s_route[i].retry = false;
	thread_mutex_unlock(&s_lock);
	for(unsigned i = 0; i < CW_ACH_SLOT_COUNT; i++) flush_slot(&s_slot[i]);
	achievements_tick();
	return !achievements_lifecycle_pending();
}

bool achievements_unbind(uint8_t slot, uint64_t generation) {
	if(slot >= CW_ACH_SLOT_COUNT) {
		__atomic_add_fetch(&s_bad_slot_drops, 1u, __ATOMIC_RELAXED);
		return false;
	}
	if(!__atomic_load_n(&s_inited, __ATOMIC_ACQUIRE))
		return false;
	uint32_t now_ms = module_now_ms(); /* the clock is never read under the lock */
	struct ach_slot* s = &s_slot[slot];

	thread_mutex_lock(&s_lock);
	enum ach_reject_reason why;
	if(!actor_ok(s, generation, &why)) {
		s->status.rejected[why]++;
		thread_mutex_unlock(&s_lock);
		return false;
	}
	/* (1) CLOSING, and (2) the drain, under ONE lock hold -- this is the R1
	 * MED 8 fix. The old code released the lock between the drain and the
	 * final clear, so an emit landing in that window either folded into a
	 * coalescer that coalescer_clear() then threw away with NO counter, or was
	 * counted under a reason that did not describe what happened. With the
	 * mark set first and never released before the drain, an emit is either
	 * IN this drain or rejected as ACH_REJECT_CLOSING -- and the clear below
	 * provably has nothing to throw away.
	 *
	 * Both rings are one FIFO shared by the two slots, so this drains the
	 * other bound actor's pending events too: exactly what the next
	 * achievements_tick() would have done a frame later, and it keeps each
	 * ring a single ordered queue [design 3 "Release / rebind order" step 2].
	 * drain_locked() empties all THREE queues -- main ring, ordered overflow
	 * ring, coalescer -- so the clear below provably has nothing to lose. */
	s->status.closing = true;
	drain_locked(now_ms);
	thread_mutex_unlock(&s_lock);

	/* (3) flush if dirty; a failure keeps the state dirty and is reported.
	 * Outside the lock: file I/O never runs under it. */
	flush_slot(s);

	/* (4) clear the transient state and release the slot. */
	thread_mutex_lock(&s_lock);
	ride_origin_reset(s);
	coalescer_clear(&s->co);
	s->status.coalesce_used = 0;
	s->notice.ach_id = -1;
	s->notice.started_ms = 0;
	s->notice.hint_active = false;
	s->status.bound = false;
	s->notice_head = s->notice_count = 0;
	s->status.closing = false;
	bool saved = !s->status.dirty;
	thread_mutex_unlock(&s_lock);
	return saved;
}

void achievements_notice_clear(uint8_t slot) {
	if(slot >= CW_ACH_SLOT_COUNT || !s_lock_ready)
		return;
	thread_mutex_lock(&s_lock);
	if(s_blk) {
		struct ach_slot* s = &s_slot[slot];
		if(s->notice_count) {
			s->notice_head = (s->notice_head + 1) % CW_ACH_COUNT;
			s->notice_count--;
		}
		s->notice.ach_id = -1;
		s->notice.started_ms = 0;
		/* hint_active is deliberately untouched: it is not a toast. */
	}
	thread_mutex_unlock(&s_lock);
}

static bool flush_all_now(void) {
	if(!__atomic_load_n(&s_inited, __ATOMIC_ACQUIRE) || !s_blk)
		return false;
	achievements_tick();
	for(int i = 0; i < CW_ACH_SLOT_COUNT; i++)
		flush_slot(&s_slot[i]);
	bool saved = true;
	thread_mutex_lock(&s_lock);
	for(int i = 0; i < CW_ACH_SLOT_COUNT; i++)
		saved = saved && !s_slot[i].status.dirty;
	thread_mutex_unlock(&s_lock);
	return saved;
}

bool achievements_flush_all(void) {
 bool cooperative = s_cooperative_saves; s_cooperative_saves = false;
 bool saved = flush_all_now(); s_cooperative_saves = cooperative; return saved;
}

void achievements_shutdown_without_save(void) {
	if(!s_blk) return;
	for(unsigned i = 0; i < CW_ACH_SLOT_COUNT; i++)
		if(s_blk->save[i].active) stats_save_cancel(&s_blk->save[i].job);
	__atomic_store_n(&s_inited, false, __ATOMIC_RELEASE);
	thread_mutex_lock(&s_lock); block_release();
	s_ring_count = s_ovf_count = s_control_overflow = 0;
	memset(s_route, 0, sizeof(s_route)); s_stats_dir[0] = 0;
	thread_mutex_unlock(&s_lock); s_init_status = ACH_INIT_NEVER_RUN;
}

/* Re-init/shutdown must close producers and drain under the SAME lock before
 * disk I/O. Otherwise an emit during the flush could be wiped by teardown. */
static bool quiesce_flush_all(void) {
	achievements_tick();
	/* A known failure requires an explicit Retry; shutdown is never one. */
	if(achievements_lifecycle_pending()) return false;
	uint32_t now_ms = module_now_ms();
	thread_mutex_lock(&s_lock);
	for(int i = 0; i < CW_ACH_SLOT_COUNT; i++) {
		s_route[i].epoch = 0;
		if(s_slot[i].status.bound)
			s_slot[i].status.closing = true;
	}
	drain_locked(now_ms);
	thread_mutex_unlock(&s_lock);
	return achievements_flush_all();
}

/* ---- diagnostics (B4 supersedes these with the public API) ------------- */

bool achievements_debug_status(uint8_t slot, struct ach_status* out) {
	if(slot >= CW_ACH_SLOT_COUNT || !out || !s_lock_ready)
		return false;
	bool ok;
	thread_mutex_lock(&s_lock);
	ok = s_blk != NULL; /* a disabled module has no per-slot status at all */
	if(ok) {
		*out = s_slot[slot].status;
		out->init_status = s_init_status;
	}
	thread_mutex_unlock(&s_lock);
	return ok;
}

bool achievements_debug_notice(uint8_t slot, struct ach_notice* out) {
	if(slot >= CW_ACH_SLOT_COUNT || !out || !s_lock_ready)
		return false;
	bool ok;
	thread_mutex_lock(&s_lock);
	ok = s_blk != NULL;
	if(ok)
		*out = s_slot[slot].notice;
	thread_mutex_unlock(&s_lock);
	return ok && out->ach_id >= 0;
}

bool achievements_notice_front(uint8_t slot, struct ach_notice* out) {
	if(slot >= CW_ACH_SLOT_COUNT || !out || !s_lock_ready)
		return false;
	bool ok;
	thread_mutex_lock(&s_lock);
	ok = s_blk != NULL;
	if(ok) {
		struct ach_slot* s = &s_slot[slot];
		*out = s->notice_count ? s->notices[s->notice_head] : s->notice;
		if(!s->notice_count) {
			out->ach_id = -1;
			out->started_ms = 0;
		}
		out->hint_active = s->notice.hint_active;
	}
	thread_mutex_unlock(&s_lock);
	/* True means "there is a TOAST": the hint is a flag on `out`, not a
	 * notice, and is read whatever this returns [design 7, R1 LOW 11]. */
	return ok && out->ach_id >= 0;
}

const struct stats_profile* achievements_debug_profile(uint8_t slot) {
	if(slot >= CW_ACH_SLOT_COUNT || !s_blk)
		return NULL;
	return &s_slot[slot].profile;
}

uint32_t achievements_debug_now_ms(void) {
	return module_now_ms();
}

uint32_t achievements_debug_bad_slot_drops(void) {
	return __atomic_load_n(&s_bad_slot_drops, __ATOMIC_RELAXED)
		+ __atomic_load_n(&s_pre_init_drops, __ATOMIC_RELAXED);
}
