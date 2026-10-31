/* Copyright (c) 2026 CaveWii_v2 contributors */
/*
 * The event seam: ONE function every producer in the engine calls when it
 * DECIDES a fact worth a statistic or an achievement.
 *
 * Spec: docs/achievements_design.md section 5.1 (the one function, the typed
 * payload, the kind list), 5.2 (the contract: who produces what, on which
 * thread, and the acceptance matrix the consumer applies) and 5.3 (the exact
 * CaveWii call sites lane A adds).
 *
 * Contract, restated so a producer never has to read the design:
 *
 *  - Emit ONCE, at the site that DECIDES the fact, never from a prediction
 *    path and never from both a request handler and a tick [design 5.2
 *    "Duplicate prevention"].
 *  - `authority` says WHO OBSERVED the fact (our server thread / our
 *    main-thread client code / a remote Beta server's Packet 200). It is NOT
 *    provenance: whether the observation lands in the `local` or the `remote`
 *    counter column is decided by the session mode given at bind time, by the
 *    consumer, never by the producer [design 5.2 "Authority vs provenance"].
 *  - `game_event_emit()` copies the event under a short mutex. It never
 *    allocates, never blocks on I/O and never calls back into the caller, so
 *    it is safe from the server thread, the network thread and the main
 *    thread. All state mutation, toast queueing and file I/O happen later, on
 *    the main thread, in achievements_tick().
 *  - Emitting before achievements_init(), for an unbound slot, or with a
 *    stale generation is SAFE: the event is dropped at drain time and counted
 *    by reason (see struct ach_status in achievements.h).
 *
 * Producers include only this header; the rules, the ring and the lifecycle
 * live behind achievements.h so a call site never pulls in the state model.
 */

#ifndef CW_GAME_EVENT_H
#define CW_GAME_EVENT_H

#include <stddef.h>
#include <stdint.h>

#include "../../entity/actor_ident.h"

/* WHO observed the fact [design 5.1]. */
enum game_event_authority {
	GE_AUTH_SERVER = 0,	   /* our local server thread decided it */
	GE_AUTH_CLIENT = 1,	   /* our main-thread client code decided it */
	GE_AUTH_PACKET200 = 2, /* a remote Beta server told us (Packet 200) */
	GE_AUTH_COUNT
};

/* Every kind design 5.1 lists, in the order it lists them. The numbering is
 * internal (the seam is in-process, never serialized), but it is stable so a
 * counter dump stays readable across builds. */
enum game_event_kind {
	GE_BLOCK_HARVESTED = 0,	  /* u.item.id = block id; only when it dropped  */
	GE_CRAFT_TAKEN,			  /* u.item = crafting result taken             */
	GE_FURNACE_TAKEN,		  /* u.item = furnace output taken              */
	GE_ITEM_PICKED_UP,		  /* u.item = item entity collected             */
	GE_ITEM_USED,			  /* u.item.id = item id                        */
	GE_ITEM_BROKEN,			  /* u.item.id = item id                        */
	GE_KILLED,				  /* sub = enum in monsters_object.h            */
	GE_KILLED_PLAYER,		  /* victim was a player                        */
	GE_DAMAGE_DEALT,		  /* u.n.amount                                 */
	GE_DAMAGE_TAKEN,		  /* u.n.amount                                 */
	GE_DIED,				  /*                                            */
	GE_JUMPED,				  /*                                            */
	GE_DROPPED,				  /*                                            */
	GE_INVENTORY_OPENED,	  /*                                            */
	GE_MOVED,				  /* sub = move kind, u.move.cm                 */
	GE_FELL,				  /* u.move.cm (only when the fall was >= 2.0)  */
	GE_RIDE,				  /* sub = mount kind, u.ride (coords + cm)     */
	GE_RIDE_END,			  /* sub = mount kind; resets the ride origin   */
	GE_FISH_CAUGHT,			  /*                                            */
	GE_PIG_FELL_WITH_RIDER,	  /* u.fall.f = fall distance in BLOCKS (float) */
	GE_GAME_STARTED,		  /* sub = enum game_event_world_kind           */
	GE_GAME_LEFT,			  /*                                            */
	GE_MP_JOINED,			  /*                                            */
	GE_TICK,				  /* sub = enum game_event_tick_mount; one per  */
							  /* bound player per tick                      */
	GE_REMOTE_STAT,			  /* u.remote; authority is ALWAYS PACKET200    */
	GE_KIND_COUNT
};

/* In-process lifecycle facts, deliberately outside the statistic table.
 * BEGIN is emitted only after admission; sub is 0 local / 1 remote.
 * END quiesces that epoch at emit, CHECKPOINT orders a main-thread save.
 * They never enter the Minecraft protocol or world archive. */
#define GE_SESSION_BEGIN 253u
#define GE_SESSION_END 254u
#define GE_SESSION_CHECKPOINT 255u

/* Snapshot at the producer's observation, never at drain. Host transport
 * generation zero is replaced by a process-lifetime achievement epoch.
 * A zero result means there is no admitted achievement owner. */
uint64_t game_event_next_epoch(void);
uint64_t game_event_owner_epoch(uint8_t slot);

/* GE_MOVED sub [design 1.4 rows 2000/2001/2005/2003/2004]. The producer
 * emits only when Beta would: cm > 0, and cm > 25 for GE_MOVE_FLY. */
enum game_event_move_kind {
	GE_MOVE_WALK = 0,  /* on ground                    -> walkOneCm  */
	GE_MOVE_SWIM = 1,  /* body AABB in water           -> swimOneCm  */
	GE_MOVE_DIVE = 2,  /* eye inside water             -> diveOneCm  */
	GE_MOVE_CLIMB = 3, /* on a ladder and dy > 0       -> climbOneCm */
	GE_MOVE_FLY = 4,   /* airborne                     -> flyOneCm   */
	GE_MOVE_KIND_COUNT
};

/* GE_RIDE / GE_RIDE_END sub [design 1.4 rows 2006/2007/2008]. */
enum game_event_mount_kind {
	GE_MOUNT_MINECART = 0, /* -> minecartOneCm, and the onARail rule */
	GE_MOUNT_BOAT = 1,	   /* -> boatOneCm                          */
	GE_MOUNT_PIG = 2,	   /* -> pigOneCm                           */
	GE_MOUNT_KIND_COUNT
};

/* GE_TICK sub: WHAT THE PLAYER IS RIDING on the tick being reported, as an
 * enum that has a value for "nothing" -- which enum game_event_mount_kind
 * deliberately does not (GE_MOUNT_MINECART is 0).
 *
 * WHY THE TICK CARRIES IT (R2 MED 4). Beta clears the minecart ride origin in
 * EntityPlayer.onUpdate, on EVERY tick the player is not riding:
 *
 *	   addStat(StatList.minutesPlayedStat, 1);          // EntityPlayer.java:123
 *	   if(ridingEntity == null)                         // EntityPlayer.java:124
 *	   {
 *		   startMinecartRidingCoordinate = null;        // EntityPlayer.java:126
 *	   }
 *
 * -- not in addMovementStat. CaveWii used to reset on GE_MOVED / GE_RIDE_END
 * instead, so a lost dismount left a stale origin that the next ride could
 * cross the onARail threshold against. The tick is the faithful carrier: it is
 * emitted unconditionally once per player per tick (the same site that feeds
 * playOneMinute, exactly as Beta's two statements sit together), so the reset
 * happens whenever Beta's would.
 *
 * The producer sets GE_TICK_NOT_RIDING when `ridingEntity == null` and
 * otherwise GE_TICK_RIDING_BASE + the mount kind. GE_MOVED no longer resets
 * anything; GE_RIDE_END still does (harmless and earlier by up to one tick). */
enum game_event_tick_mount {
	GE_TICK_NOT_RIDING = 0, /* ridingEntity == null -> reset the rail origin */
	GE_TICK_RIDING_BASE = 1, /* + enum game_event_mount_kind */
	GE_TICK_MOUNT_COUNT = GE_TICK_RIDING_BASE + GE_MOUNT_KIND_COUNT,
};

/* GE_GAME_STARTED sub. Beta's Minecraft.startWorld counts startGame on both
 * branches and createWorld or loadWorld on the branch it took [design 1.4
 * rows 1000/1001/1002]; `sub` says which branch. NOTE for lane A: startGame /
 * createWorld / loadWorld are CLIENT-owned stats, so both branches must emit
 * with GE_AUTH_CLIENT from a main-thread site -- a GE_AUTH_SERVER emit is
 * rejected as a wrong observer by the 5.2 matrix, by design. */
enum game_event_world_kind {
	GE_WORLD_NEW = 0,	 /* -> createWorld */
	GE_WORLD_LOADED = 1, /* -> loadWorld   */
	GE_WORLD_KIND_COUNT
};

/* The typed, fixed-size payload of design 5.1 -- field names, order and types
 * verbatim. No pointers, no heap, trivially copyable: the ring stores it by
 * value. */
struct game_event {
	uint8_t kind;	   /* enum game_event_kind      */
	uint8_t authority; /* enum game_event_authority */
	uint8_t sub;	   /* move / mount / world / mob kind, per `kind` */
	uint8_t pad;
	/* Same actor value ABI; generation must match the achievement binding.
	 * B2 supplies a separate session epoch for host transport generation zero,
	 * preserving it across death/dimension transfer and replacing it on leave. */
	struct actor_ident who;
	union {
		struct {
			uint16_t id;
			uint16_t count;
		} item; /* block or item id, stack count */
		struct {
			int32_t cm;
		} move; /* centimetres, Beta rounding */
		struct {
			int32_t x, y, z;
			int32_t cm;
		} ride; /* floored rider block coords, then cm */
		struct {
			int32_t amount;
		} n; /* damage amount */
		struct {
			int32_t stat_id;
			int32_t amount;
		} remote; /* Packet 200 passthrough */
		struct {
			float f;
		} fall; /* GE_PIG_FELL_WITH_RIDER: the fall distance in BLOCKS.
				 * EntityPig.fall(float f) tests `f > 5F` on the FLOAT
				 * (EntityPig.java:112), so the payload is that float and not
				 * a x100 integer: a fall in (5.0, 5.005] rounds to 500 or 501
				 * centimetres depending on the value and diverged from Beta
				 * either way (R2 MED 5). */
	} u;
};

/* ---- Size, and why it is not the 24 the design text quotes ---------------
 *
 * docs/achievements_design.md 5.1 calls this a "fixed 24-byte typed payload"
 * and section 5.2 sizes the ring from that number ("1024 entries, 24 KB").
 * That literal was computed before the identity became the S1-a2
 * `struct actor_ident`, and with that identity it is arithmetically
 * unreachable, whatever the field order:
 *
 *   4  kind + authority + sub + pad
 *   16 struct actor_ident  (uint64_t generation forces 8-byte alignment, so
 *      the struct is 8 + 1 + 7 padding = 16 ALL BY ITSELF)
 *   16 the widest union member (ride: four int32_t)
 *   -- 4 + 4 padding + 16 + 16 = 40
 *
 * 4 + 16 alone is already 20, leaving 4 bytes for a 16-byte union; a
 * __attribute__((packed)) variant is 29, still not 24, and would trade
 * unaligned 64-bit loads on PPC for nothing. So the struct below keeps every
 * field of 5.1 exactly as written and the assert pins the size the fields
 * actually produce, rather than a number the fields cannot produce. The
 * PROPERTY the 24 was standing in for -- a fixed-size, pointer-free, trivially
 * copyable payload whose size is a compile-time constant the ring is sized
 * from -- is what the asserts below actually prove, and CW_GAME_EVENT_SIZE is
 * where the ring gets its footprint (1024 * 40 = 40 KB, not 24 KB).
 * Reported to lane B lead with B2; the design text is not this brief's to
 * edit. */
#define CW_GAME_EVENT_SIZE 40u

_Static_assert(sizeof(struct actor_ident) == 16,
			   "S1-a2 actor_ident is 8-byte aligned: 16 bytes on its own");
_Static_assert(sizeof(struct game_event) == CW_GAME_EVENT_SIZE,
			   "game_event must stay a fixed-size POD the ring can size from");
_Static_assert(offsetof(struct game_event, kind) == 0,
			   "design 5.1 field order: kind first");
_Static_assert(offsetof(struct game_event, authority) == 1,
			   "design 5.1 field order: authority second");
_Static_assert(offsetof(struct game_event, sub) == 2,
			   "design 5.1 field order: sub third");

/* Copy `ev` into the consumer's ring. Never allocates, never blocks on I/O,
 * safe from any thread, and a no-op (counted) before achievements_init().
 * When the main ring is full an event that can change an achievement or a
 * notice goes to the ORDERED OVERFLOW RING (FIFO, never folded, never
 * approximated) and a pure counter folds into the emitting actor's coalescer,
 * so a stalled drain costs neither a count nor an ordering [design 5.2 v4 "No
 * routine loss"]. The class is decided at EMIT time from the kind AND the
 * payload -- CW_EVENT_CLASS_ROWS in stats_table.h plus the per-kind payload
 * refinements listed beside it -- so only the payloads Beta actually reads for
 * an achievement take the ordered path. A GE_ITEM_PICKED_UP of anything but
 * wood 17 or leather 334 reaches no statistic at all and is dropped there and
 * then (counted in ach_status.noop_events, which is not a rejection). */
void game_event_emit(const struct game_event* ev);

#endif /* CW_GAME_EVENT_H */
