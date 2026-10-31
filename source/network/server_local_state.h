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

#ifndef SERVER_LOCAL_STATE_H
#define SERVER_LOCAL_STATE_H

// Interface split (build_architecture step 7, slice 4, 2026-08-27): the
// server's FULL STATE, moved verbatim out of server_local.h. This is
// struct server_local itself plus every embedded helper struct/enum it
// contains, and the heavy include set they need. Only TUs that embed the
// struct by value or dereference its fields (server_local.c and its
// server_*.c/inventory_*.c siblings, main.c, and the CAVEWII_SERVER_LOCAL_TEST
// harness) should include this header. Everyone else -- callers that only
// hold/pass a `struct server_local*` -- includes the opaque API header,
// server_local.h, so a struct-field edit here no longer rebuilds them.

#include "residency.h"
#include "client_world_payload.h"

#include <m-lib/m-dict.h>
#include <m-lib/m-string.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h> // IMP-TE-CAPS: server_local_te_tables_init_heap

#include "../block/dispenser_logic.h"
#include "../block/furnace_logic.h"
#include "../block/piston_moving.h"
#include "../item/inventory.h"
#include "../world.h"
#include "level_archive.h"
#include "death_drop_journal.h"
#include "recipe_craft.h"
#include "map_data.h" // struct map_data (resident map cache, MAX_MAP_CACHE below)
#include "server_weather.h" // struct active_lightning_bolt
#include "server_world.h"
#include "spawner_mob_id.h" // struct spawner_te (dependency-light; not spawner.h's burst core)
#include "tile_entities_store.h" // A1b: te_store view + owner keys
#include "server_transfer.h" // A2-T/A2-W: transfer transactions, slot lifecycle
#include "../entity/entity.h"
#include "../platform/thread.h" // struct thread (embedded server-thread handle)

/* The ONE per-sample movement bound in this codebase, shared by both position
 * handlers so a guest body can never be validated against a different rule
 * than the host. Squared HORIZONTAL delta of a single SRPC_PLAYER_POS sample;
 * 2 blocks/sample is teleport scale (Beta sprint is ~0.28 blocks/tick, so even
 * a merged two-tick sample after an inbox drop stays ~0.56). Horizontal only:
 * Beta's fall speed passes 2 blocks/tick before terminal velocity, so the same
 * cap on the vertical axis would reject legitimate falling.
 *   - Host  (server_local.c, SRPC_PLAYER_POS): suppresses the Beta footstep
 *     odometer / farmland trample across a teleport-scale jump.
 *   - Guest (server_local_guest.c, server_local_guest_position): REJECTS the
 *     sample and issues a correction -- the guest body is not the host actor,
 *     and residency anchor 1 follows it. */
#define PLAYER_MOVE_TELEPORT_DELTA_SQ 4.0

struct server_rpc;

/* The client's local player entity is spawned with id 0 (both CRPC_WORLD_RESET
 * sites send local_entity = 0) and remap_source_id() reserves 0 as "the local
 * player" so a rider claim survives the save/load id remap unchanged. Server
 * code that records a rider MUST use this, not an invented id: the client gates
 * its throttle and its death-cleanup on `occupant_id == local_player->id`, so
 * any other value silently disables riding and leaks stale claims. Occupancy is
 * carried by the `occupied` flag -- this id alone does not distinguish
 * "empty" from "ridden". */
#define SERVER_LOCAL_PLAYER_ENTITY_ID 0

/* Minecart correction cadence. These conservative starting values bound the
 * shared 16-slot client inbox: ridden carts correct at 4 Hz, background carts
 * become round-robin eligible at roughly 1 Hz, and no server tick makes more
 * than two nonblocking snapshot attempts. All three are tuning candidates
 * pending observation on real Wii hardware. */
#define MINECART_RIDDEN_SNAPSHOT_INTERVAL 5u
#define MINECART_BACKGROUND_SNAPSHOT_INTERVAL 20u
#define MINECART_SNAPSHOT_ATTEMPT_BUDGET 2u

#define MAX_REGIONS 4
/* A1a: world context slots in struct server_local (Overworld + Nether). */
#define SERVER_LOCAL_WORLD_SLOTS 2
// View distance in chunks. The Wii's 24 MB budget caps this at 5; the PC build
// has the memory + GPU for a deeper view (MAX_CHUNKS below scales from this).
#ifdef PLATFORM_PC
#define MAX_VIEW_DISTANCE 10 // in chunks
#else
#define MAX_VIEW_DISTANCE 5 // in chunks
#endif
#define MAX_HIGH_DETAIL_VIEW_DISTANCE 2
// Near-send FIX #2 backoff table size (struct field below; scan/mark logic
// lives in server_local.c). Declared here, not there, because the field's
// array bound must be visible to every TU that embeds struct server_local.
#define NEAR_SEND_FAIL_TABLE_SIZE 16
// M1 deep reach: the far ring streams compact surface-only "far chunks" in the
// annulus (MAX_VIEW_DISTANCE, MAX_FAR_VIEW_DISTANCE]. They are pure-streamed
// (generated, extracted, freed same tick) so they never enter the resident set --
// MAX_CHUNKS stays sized to MAX_VIEW_DISTANCE. PC gets +1 ring; Wii keeps the far
// radius == view radius (empty annulus -> feature off) until measured on-device.
#ifdef PLATFORM_PC
#define MAX_FAR_VIEW_DISTANCE (MAX_VIEW_DISTANCE + 1)
#else
#define MAX_FAR_VIEW_DISTANCE MAX_VIEW_DISTANCE
#endif
// The far ring distance is now a RUNTIME dial (server_local.far_view_distance,
// config "lod.far_view_distance"), defaulting to MAX_FAR_VIEW_DISTANCE per platform.
// On Wii the default leaves the far ring OFF (== MAX_VIEW_DISTANCE); raising it to
// MAX_VIEW_DISTANCE+1 enables the +1 ring -- the gate for the on-device MEM1
// measurement. This cap bounds the client far-section RAM against an absurd value.
//
// 16 chunks on BOTH platforms (2026-09-03, Stage-2 reach sweep): matches Beta
// 1.7.3's "Far" render-distance setting, the ceiling the sweep
// (docs/stage2_lod_plan_2026-09-03.md 3) needs to actually test. Split by
// platform so each keeps its own "+K" offset from its own MAX_VIEW_DISTANCE
// (5 Wii / 10 PC) while landing on the SAME absolute cap -- PC's existing
// +6 already lands on 16 unchanged; only Wii's offset moves, from +6 (11)
// to +11 (16). No static array/bitmap is sized from this cap on either
// side: the far annulus is tracked by dynamic dicts (server_world.h's
// dict_far_sent server-side, world.h's dict_wsection_t sections
// client-side), generated/extracted/sent/freed per-tick rather than held in
// any fixed-size pool (server_local.c's WDS_CHUNK_SEND far-send batch), so
// raising this cap changes the far annulus SCAN's iteration bound (O((2R+1)^2)
// compute per far-send-batch tick when a runtime override actually reaches
// 16) and dynamic dict churn, not any fixed allocation -- no memory-budget
// delta to report for a value nothing shipped sets by default (config_wii.json
// keeps its existing lod.far_view_distance; this only raises the CEILING an
// explicit override -- config or --cw-far-view-distance -- can reach).
#ifdef PLATFORM_PC
#define MAX_FAR_VIEW_DISTANCE_CAP (MAX_VIEW_DISTANCE + 6) // 10+6 == 16
#else
#define MAX_FAR_VIEW_DISTANCE_CAP (MAX_VIEW_DISTANCE + 11) // 5+11 == 16
#endif
// Far columns streamed per tick, AFTER the near send. The "keep up with a walk"
// target is ~8 (far-frontier circumference / cross rate). With the CHEAP terrain-only
// far path (server_world_extract_far_skin -- 1 chunk gen ~1ms, no 9-chunk populate
// cascade) the old per-send spike that pinned this to 3 is gone, so 8/tick (~8ms of
// far gen, time-capped by FAR_GEN_BUDGET_MS) keeps the far ring filling with a walk.
// The FAITHFUL ("populated") far mode still pays the full cascade per send -- when
// that mode is selected the time budget below throttles it instead.
#define FAR_SEND_BUDGET 8
// Per-tick wall-clock cap on far-skin generation. The cheap far path runs OUTSIDE
// ensure_populated's gen throttle, so bound its time directly so FAR_SEND_BUDGET far
// gens can't overrun the tick on the single Broadway core.
#define FAR_GEN_BUDGET_MS 8
// T2 far-send share (2026-09-04, lane/far-send-share; docs/platform_perf_notes.md
// "Far-send share while the near frontier moves"). How many chunks the far ring is
// allowed to "look ahead" when it decides a column is ABOUT to enter the near radius
// and must therefore NOT be far-sent (the World38 promotion-debt guard). A column is
// skipped when the player's CURRENT chunk-crossing heading, extrapolated this many
// chunks, would put it inside the near square. 1 chunk = "it becomes near on the very
// next crossing"; the far ring's innermost row directly ahead of the player is exactly
// that set, and it is also the only set that can accumulate un-evictable far_sent
// claims while walking. Compile-time, not config: it is a property of the geometry
// (one crossing), not a tuning dial. Inert unless the share path below is enabled.
#define FAR_SEND_SHARE_LEAD_CHUNKS 1
#define FAR_EVICT_BUDGET 8 // far unloads per tick (capped so a teardown burst can't
						   // fill the shared inbox and stall near/entity sends)
#define NEAR_EVICT_BUDGET 8 // resident unloads per tick; mirrors far eviction so
							// a teardown burst never blocks on a full client inbox
#define NEAR_UNLOAD_PENDING_MAX 16
/* SAVE BATCH cadence (2026-09-07 far_walk pace regression). How many server
 * ticks one region-header publication may cover. The journaled publication
 * costs three fsyncs plus a file create/rename/remove -- on the Wii that runs
 * on the server thread inside libfat -- so its cost per SAVED CHUNK is that
 * fixed price divided by the number of chunks in the batch.
 *
 * Why not 1 (a per-tick batch): MEASURED on the far_walk_6m/seed-42 PC harness,
 * the profile writes ~0.43 chunks per tick (1,976 committed writes over 4,550
 * ticks) -- the eviction loop's NEAR_EVICT_BUDGET of 8 is almost never taken,
 * and the autosave sweep is one chunk per tick by design. A per-tick batch is
 * therefore a batch of ONE and amortizes nothing; it only removes the
 * redundant syscalls (1,441 -> 945 us per committed write, -34%). Spanning
 * ~1 second of ticks gives the batch real content and takes the fixed price
 * per write down with it.
 *
 * Why not larger: an OPEN batch is unpublished. Its chunk payloads are on the
 * card but the on-disk index still names the previous ones, so a power cut
 * inside the window reverts those chunks to their previous saved contents.
 * This bounds that window to 40 ticks = 2 seconds, which is Beta 1.7.3's own
 * autosave cadence (GameSettings: autosave every 40 ticks, "Default (2s)"),
 * so the exposure equals Beta's; the earlier 20-tick (~1 s) window paid twice
 * the libfat fsyncs on the real Wii (far_walk 42 fv6: evict 50 s / commit 42 s
 * vs the pre-journal 12 s / 11 s) for a window Beta never promised.
 * It is still strictly MORE durable than
 * the code was before the journal landed (ea0de5b3): dab600e9 fsynced nothing
 * on the chunk-write path at all and relied entirely on the explicit
 * region_archive_sync barriers, whose spacing is measured in autosave
 * intervals. REGION_BATCH_MAX caps the batch by size as well, so a burst
 * publishes early rather than growing the window. */
#define SAVE_BATCH_MAX_TICKS 40
// W1-c (far-reach program Wave 1, docs/far_reach_program_2026-09-03.md): upper
// bound on the cached far-pick frontier template (see server_local.c's far
// send loop). The template holds every (dx,dz) offset in the square
// [-far_view_distance, far_view_distance]^2 sorted nearest-first; sized for
// the largest far_view_distance this build can ever run (MAX_FAR_VIEW_
// DISTANCE_CAP), never resized at runtime.
#define FAR_PICK_TEMPLATE_MAX \
	((2 * MAX_FAR_VIEW_DISTANCE_CAP + 1) * (2 * MAX_FAR_VIEW_DISTANCE_CAP + 1))
// Named (not anonymous) so server_local.c's qsort comparator can cast to it.
struct far_pick_offset {
	int16_t dx, dz;
};
#define MAX_CHUNKS ((MAX_VIEW_DISTANCE * 2 + 2) * (MAX_VIEW_DISTANCE * 2 + 2))
#define MAX_HIGH_DETAIL_CHUNKS ((MAX_HIGH_DETAIL_VIEW_DISTANCE * 2 + 2) * (MAX_HIGH_DETAIL_VIEW_DISTANCE * 2 + 2))
/* IMP-TE-CAPS (2026-10-21): the seven tile-entity tables are growable MEM2
 * slabs (tile_entities_store.h "growable backing"); these are only their
 * INITIAL row counts, never a limit. MAX_CHESTS / MAX_SIGNS also remain the
 * fixed row count of the legacy flat archives (chests.dat / signs.dat image
 * format, complex_block_archive.c): the tables start at least that large, so
 * every flat-image row index stays addressable. */
#define MAX_CHESTS 256
#define MAX_CHEST_SLOTS 54
#define MAX_SIGNS 256
#define MAX_FURNACES 64 // initial furnace rows (grows on demand)
#define MAX_DISPENSERS 64 // initial dispenser rows (grows on demand)
#define MAX_NOTEBLOCKS 64 // initial note-block rows (grows on demand)
#define MAX_JUKEBOXES 64  // initial jukebox rows (grows on demand)
#define MAX_ACTIVE_SPAWNERS 8 // dungeon spawners ticked near the player at once
// FH-17: resident MobSpawner tile entities tracked at once (same order/table
// shape as dispensers/noteblocks -- see server_spawner.h). Distinct from
// MAX_ACTIVE_SPAWNERS above: that is the small near-player refire-timer
// tracker; this is the persistent position-keyed store every loaded spawner's
// mob type/delay lives in, mirroring furnace_pos/dispenser_pos's lifecycle
// (populated by dungeon worldgen or chunk load, released on chunk evict).
// Initial rows: one spawner per chunk in TWO maximum residency footprints
// (PC 2*22*22 = 968; Wii 2*12*12 = 288); grows on demand like the others.
#define MAX_SPAWNER_TES (2 * MAX_CHUNKS)

/* A1b (split-dimensions D9): the owner keys of the seven tile tables, ONE
 * session-owned set shared by both worlds (no duplicated payload arrays).
 * Row i of table k is owned by world slot `*_slot[i]` instance `*_gen[i]`
 * (see struct te_owner_view in tile_entities_store.h for the free-row /
 * unstamped-row rules): 5 bytes per row. IMP-TE-CAPS: each array is a
 * growable slab of its kind (same row count as that kind's table). */
struct te_owner_table {
	uint32_t* chest_gen;
	uint32_t* sign_gen;
	uint32_t* furnace_gen;
	uint32_t* dispenser_gen;
	uint32_t* note_gen;
	uint32_t* record_gen;
	uint32_t* spawner_gen;
	uint8_t* chest_slot;
	uint8_t* sign_slot;
	uint8_t* furnace_slot;
	uint8_t* dispenser_slot;
	uint8_t* note_slot;
	uint8_t* record_slot;
	uint8_t* spawner_slot; // world slot (0/1), NOT row index
};
// Resident map-data cache (map part 2a; see item_map.c). Beta's own
// MapStorage.loadedDataMap is an UNBOUNDED in-memory hashmap for the whole
// SP session -- ItemMap.onUpdate's getMapData lookup (EVERY map stack in
// inventory, held or not, every tick) is cheap there because it almost
// always hits that cache rather than touching disk. This engine has no
// such session-lifetime object store (the exact "part 2 ItemMap seam"
// MAPDATA_SLICE_NOTES.md flagged), so a small BOUNDED, LRU-by-touch-counter
// cache stands in: large enough that carrying 1-2 maps never round-trips to
// SD every tick, small enough (4 * ~16.4KB colors buffer =~ 66KB) not to
// dominate the MEM1 budget an unbounded cache could. A cache miss/eviction
// is NEVER a correctness bug -- only a perf cost -- because a dirty entry is
// always flushed to disk before reuse and a miss falls back to
// map_data_archive_load (or a fresh allocation, item_map.c's
// item_map_on_update); this sizing is an UNMEASURED, conservative starting
// point (see docs/deviations.md), not a proven-sufficient one.
#define MAX_MAP_CACHE 4

// Beta EntityLiving drowning: air starts at 300 (maxAir), decrements one per
// submerged tick; the first 1-heart hit lands at air == -20, then every 20
// ticks. (The HUD bubble count = (oxygen - OXYGEN_THRESHOLD)/32 spans the
// 300..-20 range to exactly 10 bubbles.)
#define MAX_OXYGEN 300
#define OXYGEN_THRESHOLD -20
// Beta EntityPlayer.fireResistance: ticks of fire-block contact before ignition.
#define PLAYER_FIRE_RESISTANCE 20

struct complex_block_pos {
	int x, y, z;
};

struct chunk_entities;

// A dungeon mob-spawner currently within range of the player. Tracked across
// spawner cycles so each keeps its own refire delay (Beta's per-tile-entity
// timer); evicted when the player leaves range or its chunk unloads.
struct active_spawner {
	w_coord_t x, y, z;
	// FH-17: engine monster id this spawner emits, resolved from its
	// MobSpawner tile entity (spawner_te_pos[]/spawner_tes[] above) via
	// spawner_mob_from_id -- NOT from the block's metadata nibble.
	int mob;
	int delay; // spawner-cycles until the next burst
	bool used;
	bool seen; // matched by the current scan (else evicted)
	/* A1b: the world instance this tracker row was sighted in (the tracker
	 * is shared by both worlds; a same-coordinate spawner in the other world
	 * has its own row, delay and mob). */
	uint8_t world_slot;
	uint32_t world_generation;
};

// Diagnostic: chunk-eviction-loop wedge-guard trips (see server_local.c). 0 = healthy.
// CR-08 (2026-08-20): kept as a plain uint32_t extern (NOT a getter function) --
// tests/test_near_evict_defer.c resets its twin directly between cases from
// single-threaded test code, so an opaque accessor would break that legitimate
// direct-write use. The genuinely cross-thread accesses (the server thread's
// increment in server_local.c, and the main-thread/watchdog-thread reads in
// main.c/screen_ingame.c/wii/watchdog.c) each go through __atomic at their own
// call site instead -- same correctness fix, without changing this field's
// linkage or the type test code relies on.
extern uint32_t g_evict_stuck;
// Diagnostic: near-evict work deferred because the client inbox token pool was
// exhausted -- a non-far unload RPC queued for next tick, OR (stage22-0927) an
// evicting save of a chunk with live entities deferred before writing because
// its entity-destroy batch + unload could not be admitted this tick. Non-zero
// means the server avoided blocking/failing; it is never a save failure.
// CR-08: same reasoning as g_evict_stuck above (and the same test dependency,
// tests/test_near_evict_defer.c, which directly resets/reads this one).
extern uint32_t g_near_evict_deferred;

struct portal_teleporter_job;

/* Immutable comparator keys for one Beta entity-query call. Pointers remain
 * live for the duration of a piston moving-TE visit; positions/bounding boxes
 * are intentionally read live as earlier candidates move. */
/* next-gaps-0924/W2 P4b: one Beta Chunk.entities[section] list. `ids` are
 * entity ids in append (= list_order) order; SERVER_ENTITY_SECTION_PLAYER
 * stands for the host player. Owned by struct server_local.entity_sections
 * (server_local.c, the section index). */
#define SERVER_ENTITY_SECTION_PLAYER UINT32_MAX
/* v5 (ASTRA-V4-REVIEW guest query membership): the admitted guest body's
 * own Chunk.entities[] entry -- Beta's second EntityPlayerMP is a section
 * member like the host (Chunk.addEntity via World.updateEntity). */
#define SERVER_ENTITY_SECTION_GUEST (UINT32_MAX - 1u)
struct server_entity_section {
	uint32_t* ids;
	size_t count;
	size_t capacity;
};
DICT_DEF2(dict_entity_sections, int64_t, M_BASIC_OPLIST,
		  struct server_entity_section*, M_POD_OPLIST)

struct server_entity_query_ref {
	struct entity* entity;
	uint32_t id;
	int32_t chunk_x;
	int32_t chunk_y;
	int32_t chunk_z;
	uint64_t list_order;
	bool player;
	uint8_t actor; /* v5: with `player`, 0 = the host body, 1 = the guest */
};

// Server-side ownership barrier for a live dimension switch. It is armed
// immediately before CRPC_BEGIN_WORLD_SWITCH and remains armed until the
// destination/rollback CRPC_PLAYER_POS has been queued, or a pre-teardown
// failure cancels back to the still-live source world.
enum server_world_switch_phase {
	SERVER_WORLD_SWITCH_IDLE = 0,
	SERVER_WORLD_SWITCH_PORTAL,
	SERVER_WORLD_SWITCH_DEATH,
};

// FIX #3(b): autosave-sweep "already saved this epoch" set. A chunk id lands
// here the tick the autosave sweep actually saves it; the sweep's scan skips
// any id already present even if simulation re-dirtied that chunk since. See
// the field doc on server_local.autosave_saved_this_epoch below for why this
// exists (a redstone-clock/fluid chunk that re-dirties every tick used to
// keep the sweep armed, and re-SAVING, forever instead of once per interval).
DICT_DEF2(dict_autosave_saved, int64_t, M_BASIC_OPLIST, uint8_t, M_BASIC_OPLIST)

/* S1-d (mp-split-0912/68): per-ACTOR accepted-gameplay-action tallies. The
 * split_actions_6m console profile has to prove that a guest's dig/place/
 * hotbar/use land on the GUEST's own body while the host does nothing -- and
 * has to FAIL when they do not (Astra gate-3: "the new profile must fail when
 * expected per-owner effects are absent"). Nothing else can show that: the
 * existing anchor evidence is residency, not agency.
 *
 * WHAT EACH SLOT COUNTS (the increments are in server_local.c's `_for`
 * handlers, on the ACCEPTED path only -- a request dropped by the owner gate,
 * the actor barrier or the handler's own precondition counts nothing):
 *   DIG    a finished SRPC_BLOCK_DIG that actually REMOVED a block.
 *   PLACE  an SRPC_BLOCK_PLACE whose held item actually PLACED a block
 *          (onItemPlace returned true); a right-click that only activated the
 *          clicked block, or used an item on it, is not a placement.
 *   ATTACK an SRPC_ENTITY_ATTACK that passed the actor barriers and resolved
 *          to a LIVE entity (before each branch's own reach/visibility gate --
 *          the profile reports this one, it never requires it, because no mob
 *          is guaranteed to exist).
 *   USE    an SRPC_USE_ITEM that reached the held-item dispatch, or an
 *          SRPC_ENTITY_USE that resolved to a live entity. This is the one
 *          slot that counts an ACCEPTED VERB rather than a world EFFECT: a
 *          block item used into open air is legitimately inert in Beta, so
 *          there is no effect to count and refusing to count it would make a
 *          correctly-routed guest action invisible.
 *   HOTBAR an SRPC_HOTBAR_SLOT that actually re-selected the hotbar slot.
 *   DROP   an SRPC_DROP_ITEM that actually spawned an item entity.
 * SRPC_PLAYER_STATE (the eighth per-actor verb, S1-a2) is deliberately NOT
 * counted: it is a body-state sync, not an action.
 *
 * Counters saturate rather than wrap: a wrapped tally would read as "fewer
 * actions" to the collector, which is the one direction that could turn a
 * broken run into a PASS. */
enum server_player_action {
	SERVER_ACTION_DIG = 0,
	SERVER_ACTION_PLACE,
	SERVER_ACTION_ATTACK,
	SERVER_ACTION_USE,
	SERVER_ACTION_HOTBAR,
	SERVER_ACTION_DROP,
	SERVER_ACTION_COUNT,
};

/* G8-BLAST-VELOCITY v3/v4/v5: one accepted movement packet's server half
 * (NetServerHandler.handleFlying), replayed at the tracker phase -- after
 * every world's entity/tile work and the hazards, i.e. where Beta's network
 * phase runs (MinecraftServer.java:328-341).
 *
 * v5 (ASTRA-V4-REVIEW (2) queue ruling): admission is LOSSLESS. The local
 * drain (svin_process_owned_messages_admitted) takes at most
 * BETA_READ_PACKETS_PER_TICK messages per source per update -- Beta
 * NetworkManager.java:203-206 `var1 = 100; while(... && var1-- >= 0)` =
 * 101 -- and HOLDS the rest (token and per-source order kept) for the next
 * update, as Beta leaves them in readPackets. A movement sample whose body's
 * queue is full is held the same way (server_local_admit_owned), never
 * refused, never stepped early. The queue is sized to the per-update bound
 * so a simulating update can never hold for lack of room: each update's
 * events are all replayed by that update (a pause-held update keeps only
 * damaging landings, and backpressure covers that backlog). */
#define BETA_READ_PACKETS_PER_TICK 101
#define ACTOR_MOTION_QUEUE BETA_READ_PACKETS_PER_TICK
/* Reuse the queue-count byte while its batch is replaying: lethal damage
 * must keep that batch's credits until the corpse remainder finishes. */
#define ACTOR_MOTION_REPLAYING (ACTOR_MOTION_QUEUE + 1)
struct actor_motion_event {
	double px, py, pz; /* Beta lastPos (the pose before the sample) */
	double nx, ny, nz; /* the accepted new pose */
	/* v4 (ASTRA-V3-REVIEW P1 damage ordering): handleFalling's damage
	 * (NetServerHandler.java:211-213 -> EntityLiving.fall) for this packet,
	 * computed from the client-sample accounting at receive time but
	 * APPLIED at replay, after this packet's physics and after the world's
	 * entity hits. land_* = the landing pose (eye y) for the step sound. */
	double land_x, land_y, land_z;
	float rvx, rvz;    /* riding input (Packet10 x/z with -999 y/stance) */
	int16_t fall_damage; /* 0 = no damaging landing on this packet */
	bool physics;      /* false: accepted while the local session is paused --
	                    * no motion evolution (the local pause contract); only
	                    * a deferred landing, if any, is carried */
	bool on_ground;    /* the PREVIOUS packet's onGround (:211) -- normal */
	bool packet_on_ground; /* THIS packet's onGround -- riding (:90) */
	bool sneaking;     /* isSneaking() at handling time */
	bool riding;       /* ridingEntity != null at receive (:71) */
	/* v5 (ASTRA-V4-REVIEW riding): WHICH vehicle, recorded at receive --
	 * i.e. in FIFO order with the mount/dismount/interact RPCs drained
	 * around this packet -- and its seat then (the fallback seat when the
	 * vehicle is gone by the replay). */
	uint8_t vehicle_type;
	uint32_t vehicle_id;
	double seat_x, seat_y, seat_z;
	bool sleeping;     /* isPlayerSleeping branch (:112-116) */
	bool credited;     /* V6 F1 round 3: holds a movement credit */
};

struct server_local {
	/* Tick-first pacing (2026-09-10, Astra TPS diagnosis): with two moving
	 * anchors the loop ran a FULL streaming pass (evict, near scan,
	 * ensure_populated 14 ms, far 8 ms, sends) between every tick and fell to
	 * 11 ticks/s with the pending-tick counter pinned at its cap. While ticks
	 * are backlogged, one of two iterations runs LEAN (no generation / far
	 * work, short eviction quota), so the next due tick starts sooner; the
	 * other backlogged iteration and every caught-up iteration run the full
	 * pass so streaming can never starve (3 of 4 starved the frontier). 0 = default budget. */
	bool stream_lean;
	int far_gen_budget_ms;
	int near_evict_budget;
	/* Configured only with the server thread stopped. Debt is server-owned. */
	int anchor_sep;
	/* colcap-1006 (ASTRA-CC-R5 item 3): the synthetic second anchor's LIVE
	 * +X offset (0..anchor_sep). anchor_sep keeps meaning "two-anchor mode";
	 * the split_regrow_8m route moves the anchor in and out through this
	 * (main thread writes, server thread reads; relaxed atomics). Equal to
	 * anchor_sep unless a route changed it. */
	int anchor_offset;
	/* MAINTAINER RULING 2026-09-11: the split-screen SERVER load radius (config
	 * split.server_view_radius, default 4). Applies only while two anchors
	 * are active (server_local_two_anchor); single player keeps
	 * MAX_VIEW_DISTANCE. Read through server_local_view_radius(). */
	int split_view_radius;
	/* Single-player far view distance saved while the split policy runs the
	 * far ring off (effective far = the split radius); restored on leave. */
	int far_view_configured;
	/* Experimental local split-screen ("experimental.local_splitscreen", OFF
	 * by default). Set with the server thread stopped (boot:
	 * server_local_set_split_enabled) or BY THE SERVER THREAD ITSELF between
	 * ticks (live: SRPC_SPLIT_SET, Astra option A) -- never from another thread
	 * while the server runs. Its only effect on its own is that the residency
	 * scheduler is allocated, so admitting a guest body never allocates in the
	 * middle of a tick. With the switch off no NEW guest body can be admitted,
	 * so every two-anchor path below stays exactly as unreachable as it was
	 * before this field -- but a body admitted while it was ON keeps playing
	 * (and keeps the scheduler) until its normal release. */
	bool split_enabled;
	/* A guest body is admitted to gameplay: residency anchor 1 follows THAT
	 * body instead of anchor_sep's synthetic +X offset, and the guest source
	 * may stream position updates. Cleared by every guest release. */
	bool guest_gameplay;
	/* Owner-0 same-world death transaction. The retained player coordinates
	 * remain the simulation corpse; x/z below are a stream-only destination
	 * until a matching readiness ACK commits the fresh body. */
	enum host_respawn_phase {
		HOST_RESPAWN_PHASE_IDLE = 0,
		HOST_RESPAWN_PHASE_PREPARED,
		HOST_RESPAWN_PHASE_LOADING,
	} host_respawn_phase;
	uint64_t host_respawn_epoch_counter, host_respawn_epoch;
	double host_respawn_death_x, host_respawn_death_y, host_respawn_death_z;
	double host_respawn_x, host_respawn_y, host_respawn_z;
	bool host_respawn_keep_bed, host_respawn_anchor, host_respawn_ready;
	/* Shared-entity-store key the client installs the guest body under.
	 * Allocated from the ordinary server id counter so it can never collide
	 * with a server-spawned entity; 0 while no body is admitted. */
	uint32_t guest_entity_id;
	struct residency_scheduler* residency;
	struct residency_anchor residency_anchors[RESIDENCY_ANCHORS];
	uint32_t anchor_near_owed[2], anchor_oldest[2];
	/* residency-1005/2: round-robin cursor of the destination-request
	 * loader pick (server_local_rz_pick): which body is served first. */
	uint8_t rz_rr;
	/* residency-1005/4 closure item 8: per ACTOR SLOT (0 host, 1 guest)
	 * session evidence that outlives any one body -- counted at each event
	 * (server_local.c RZ_SESS), never folded from a resettable body. */
	struct rz_session_slot {
		uint32_t epoch, attempt_seq, attempt;
		uint32_t last_attempt_ticks, last_attempt_result;
		uint32_t terminal[8]; /* immutable failure + bound retry/completion */
		uint32_t sum[8]; /* RZ_SESS_* cumulative counters */
		uint32_t service_delay_max, latency_max, recovery_ticks_max;
		uint64_t moved_dm;
		uint64_t key_guest_gen, key_respawn;
		uint32_t key_world_gen, key_deaths;
		uint8_t key_slot;
		bool key_valid, key_loading;
		double lx, lz;
		uint64_t moved_at_last;
	} rz_sess[2];
	uint32_t rzi[4][5];
	uint32_t rzi_count;
	/* closure item 12: the armed runtime fault (one at a time) */
	struct {
		bool armed, entered;
		uint8_t actor, world_index;
		uint32_t world_generation, attempt;
		uint64_t guest_generation;
	} rz_fault;
	uint32_t rzf;
	/* Observe-only damage history; server writes, heartbeat uses atomic reads.
	 * Kept outside the respawned player state so counts survive every body. */
	uint32_t player_deaths, player_death_cause, player_damage_cause;
	/* Telemetry only; session history outlives either respawned body. */
	uint32_t death_telemetry[5]; /* size tied to accessor in server_local.c */

	/* Server-thread-owned route. The provider retains the actor's allocation;
	 * detach must succeed before releasing it or switching the shared world. */
	struct server_player* guest_actor;
	uint64_t guest_generation;
	bool guest_owned;
	uint64_t guest_ticket;
	bool guest_snapshot_sent, guest_confirmed;
	/* Server-owned publication retries; never retain client/actor pointers. */
	struct {
		bool pending;
		uint64_t ticket, generation;
		struct server_world_handle world_handle;
		int result;
	} guest_accept_publish;
	struct {
		bool pending;
		uint64_t ticket, generation, epoch;
		struct server_world_handle world_handle;
	} guest_respawn_publish;
	enum guest_respawn_phase {
		GUEST_RESPAWN_IDLE = 0,
		GUEST_RESPAWN_PREPARED,
		GUEST_RESPAWN_LOADING,
	} guest_respawn_phase;
	uint64_t guest_respawn_epoch_counter, guest_respawn_epoch;
	/* A2-T guest JOIN client-capacity round trip (lane C, a2t-guest-0927):
	 * a Join into a world that is not the host's own reserves the client's
	 * capacity first (CRPC_CAPACITY_REQUEST RESERVE -> SRPC_CAPACITY_REPLY),
	 * exactly like the transfer; the body is created only on the answer. */
	struct {
		bool pending;          /* a RESERVE is out; the Join waits */
		uint64_t join_ticket;  /* the Join's own ticket */
		uint32_t request_id;   /* GUEST_JOIN_CAP_ID_BIT | counter */
		uint32_t counter;
		uint8_t slot;          /* the admission world's slot */
		bool admitted_world;   /* this Join brought that world up */
		uint16_t wait_ticks;
		uint32_t timed_out_id; /* the last RESERVE that timed out: a late OK
		                        * for exactly it is RELEASEd */
		uint8_t timed_out_slot;
		/* Latest-timeout OK plus the already-pending Join may both need
		 * cleanup. No new Join starts while either known ticket is held. */
		uint32_t cleanup_ticket[2];
	} guest_join_cap;
	/* residency-1005/6 R2: a confirmed Join whose body's own column is not
	 * yet published (guest_terrain_ready) waits for its terrain at most
	 * SERVER_TRANSFER_CLIENT_WAIT_TICKS server updates, counted from the
	 * first terrain denial. At the bound the wait TERMINATES: `failed` makes
	 * every later GAMEPLAY of this Join generation inert (a late request can
	 * never revive it), the body's stream demand and residency state are
	 * released, and the client gets LOCAL_GUEST_JOIN_LOADING_FAILED. A
	 * deliberate re-Join (new generation) starts clean; guest_release resets. */
	struct {
		uint16_t wait_ticks;
		bool armed, failed;
	} guest_join_wait;
	uint64_t guest_respawn_generation;
	/* Exact admitted Overworld generation for this respawn. Never infer the
	 * target from the dead actor's still-bound world after preflight. */
	struct server_world_handle guest_respawn_target;
	/* Cross-world respawn admission. The client reserves target capacity and
	 * installs its landing core before any death-world inventory drop. */
	struct {
		bool pending, reserved, landing_armed, landing_ready, committed,
			cleanup_cancel;
		uint32_t counter, request_id, ticket, timed_out_id, cleanup_ticket;
		uint8_t timed_out_slot;
		uint16_t wait_ticks;
		/* residency-1005/5: ticks this plan has spent in LOADING without
		 * committing; bounded by SERVER_TRANSFER_CLIENT_WAIT_TICKS, then the
		 * plan terminates into the visible retryable failure. */
		uint16_t loading_ticks;
	} guest_respawn_cap;
	double guest_respawn_death_x, guest_respawn_death_y, guest_respawn_death_z;
	double guest_respawn_x, guest_respawn_y, guest_respawn_z;
	bool guest_respawn_keep_bed, guest_respawn_anchor, guest_respawn_ready;
	struct random_gen rand_src;
	// Block-container contents use a private cosmetic stream, matching Beta's
	// BlockChest-owned Random.  Spilling a chest/furnace must not perturb the
	// world RNG that drives explosion drop rolls and later simulation.
	struct random_gen container_drop_rng;
	// BlockDispenser's OWN `private Random random` field (decompile 740c583) --
	// used ONLY by its onBlockRemoval item scatter. A distinct stream from
	// BOTH rand_src (the world/simulation stream) and container_drop_rng (the
	// chest/furnace container-spill stream, which implements a DIFFERENT,
	// simpler one-stack-per-slot drop model the dispenser's own
	// onBlockRemoval override does not use). See
	// server_local_dispenser_scatter_contents.
	struct random_gen dispenser_break_rng;
	// Shared stand-in for Beta's per-entity `rand` (Entity.java:41 `new
	// Random()`) used by entities not yet migrated to their own gameplay_rng
	// (furnace-cart fuel roll, arrow unstick rolls) -- never World.rand, so
	// these draws must not advance rand_src. Dropped items no longer use it
	// (item-rng-1003). Seeded in server_local_create; users lazily guard zero.
	struct random_gen entity_rng;
	/* SERVER-WIDE UPDATE BARRIER. Set by a committed immediate respawn and
	 * cleared at the next logical update: it ends the current logical update so
	 * no later entity/hazard/world work can touch the replacement body in the
	 * same simulation pass. This stays server-wide ON PURPOSE (S1-a2): a host
	 * respawn evicts/reloads the world, which aborts the update for EVERY body,
	 * guest included -- that is not P1/P2 coupling, it is the world changing
	 * under both. What WAS wrongly coupled is the per-body INPUT latch, and
	 * that is now struct server_player.reject_pos_this_update below: this flag
	 * no longer gates any actor's queued position/state samples. Astra gate-1
	 * item 2 ("its server-wide reject_player_pos_this_update must not couple P1
	 * and P2") is satisfied by that split, not by making the barrier per-actor.
	 * Cross-thread input ownership uses the acknowledged epoch, as before. */
	bool reject_player_pos_this_update;
	/* G8-BLAST-VELOCITY v3: server-physics events abandoned because a
	 * collision query could not allocate (never partially clipped). */
	uint32_t actor_motion_incomplete;
	/* v5: messages taken from each source (0 host, 1 guest) by THIS update's
	 * admitted drain -- Beta's per-connection 101-packet budget. */
	uint16_t rpc_taken[2];
	/* v5: a movement sample that reached enqueue with a full queue. The
	 * admitted drain makes this unreachable (it holds such a sample); it is
	 * a counted invariant violation, asserted zero by the tests. */
	uint32_t actor_motion_overflow;
	// Monotonic nonzero token on each authoritative player/world handoff, plus the
	// token whose client acknowledgement is still pending. While pending, every
	// gameplay RPC belongs to the old/ambiguous epoch and simulation is suspended.
	uint32_t input_epoch_counter;
	uint32_t input_ack_pending;
	uint8_t input_handoff_retry_count;
	uint8_t input_handoff_clean_ticks;
	/* Nonzero from an accepted stable-world SRPC_STREAM_RECOVER through its
	 * replacement marker ACK/NACK cycle. It preserves simulation state and lets
	 * an exactly-owned WINDOW_CLOSE resolve an OPEN that crossed the reset FIFO. */
	uint32_t stream_recovery_epoch;
	/* One-shot fatal load state: collision loading or the respawn drain failed.
	 * server retains every ownership/simulation barrier until process restart. */
	bool load_failure_terminal;
	/* G9 R3: the light-stop terminal was published (once) for slot
	 * light_stop_slot. */
	bool light_stop_published;
	uint8_t light_stop_slot;
	/* G9 R3: requests dropped after a stop (tests, F3). */
	uint32_t light_rpc_dropped;
	// Named type (was anonymous) so code can hold a typed pointer to "a
	// player" -- the MP0 seam below resolves the player by POSITION today and
	// becomes an argmin over players[] when splitscreen lands.
	struct server_player {
		double x, y, z;
		float rx, ry;
		/* Last accepted client moveEntity state. Pistons must use the
		 * pre-call onGround/isSneaking values; geometry reprobes are not an
		 * equivalent substitute at ledges or during a landing edge. */
		bool on_ground;
		bool sneaking;
		/* Beta gates the footstep odometer on `this.ridingEntity == null`
		 * [740c583 Entity.moveEntity:478]. Maintained by the single occupancy
		 * writer so it cannot drift from the carts' own flags. */
		bool riding_minecart;
		/* Same Beta gate, boat side. Maintained ONLY by
		 * server_local_set_boat_occupancy (the single writer -- see the
		 * riding_minecart comment above; the pre-existing footstep bug this
		 * pattern fixed for carts applies identically to boats). */
		bool riding_boat;
		/* O16 (R06 P1): this actor's OWN Random -- Beta Entity.rand, one
		 * `new Random()` per entity [CFR Entity.java:96]. The G6 death scatter
		 * (EntityPlayer.dropPlayerItemWithRandomChoice -> this.rand, CFR
		 * EntityPlayer.java:300-304) draws ONLY from here, never from the
		 * world stream s->rand_src. Seeded lazily (seed 0 = unseeded) by
		 * server_local_player_rand; generator/seeding deviation in
		 * docs/deviations.md "Per-player Random". */
		/* F4-RNG (RNG ruling 8): java.util.Random, ONE per body LIFETIME --
		 * `seeded` is cleared wherever Beta constructs a fresh EntityPlayer
		 * (respawn, fresh load, guest re-admission/respawn), so a dying
		 * body's stream never continues into its replacement. Draws: the
		 * fresh hurt/death pitch, the death scatter, dropPlayerItem. */
		struct server_body_rand {
			struct java_random r;
			bool seeded;
		} entity_rand;
		/* R1d: start_seq of this actor's last ACCEPTED dig-start (0 = none);
		 * see server_interface.h block_dig.start_seq. Zero-initialised with
		 * the actor; the client id is process-monotonic, so no reset needed. */
		uint32_t last_dig_start_seq;
		/* Rider steering SOURCE (docs/deviations.md "Boat rider steering"):
		 * the client-REPORTED horizontal input velocity from SRPC_PLAYER_POS
		 * vel_x/z -- Beta's riddenByEntity.motionX/Z, re-derived fresh by
		 * the client every tick [740c583: EntityBoat.onUpdate]. Copied onto
		 * the occupied boat's own data.boat.rider_vel_x/z every server tick
		 * (entity_boat.c boat_tick_server) right before boat_advance reads
		 * it; consumed ONLY while riding_boat. Continuously overwritten,
		 * never decayed or persisted -- DISTINCT from vel_x/vel_z below
		 * (server-authored KNOCKBACK, decayed server-side): this is a raw
		 * client input signal, that is a server-owned physics quantity. */
		float rider_vel_x, rider_vel_z;
		/* Beta EntityPlayer.fishEntity [740c583]: the active fishing bobber,
		 * or 0 (none) -- id 0 is never a real dict_entity key (reserved for
		 * the local player, server_local_gen_id), so it is a safe sentinel.
		 * Set by the cast dispatch, cleared by the reel dispatch AND by
		 * entity_fish.c's own tick on every path that removes the hook
		 * (angler-invalid, 1200-tick stuck despawn) -- mirrors Beta's own
		 * `angler.fishEntity = null` at every one of EntityFish's exits.
		 * ALSO cleared at both server_local lifecycle boundaries, exactly
		 * like riding_minecart immediately above and for the identical
		 * reason (adversarial-review findings 1/2): server_local_init
		 * (this struct is an uninitialized stack automatic in main.c) and
		 * server_local_release_world_impl (a stale nonzero id must not
		 * outlive its world -- every ENTITY_FISH is bulk-freed there and
		 * next_entity_id resets to 0, but the bare scalar id would
		 * otherwise survive into the next world/dimension). */
		uint32_t fish_entity_id;
		int32_t chunk_coord_x;
		int32_t chunk_coord_y;
		int32_t chunk_coord_z;
		uint64_t chunk_list_order;
		bool added_to_chunk;
		/* v5: the world slot whose section index holds the membership
		 * above (the guest's; the host keeps its pre-v5 rule). */
		uint8_t chunk_world_slot;
		enum world_dim dimension;
		/* A2-T/A2-W: the world slot this body is COMMITTED to (Astra A2 design
		 * sec. 2: only COMMIT moves it). server_local_world_of resolves it;
		 * 0 = the routed world, as every body was before A2. */
		uint8_t world_slot;
		bool has_pos;
		bool finished_loading;
		/* PER-ACTOR INPUT LATCH (S1-a2, Astra gate-1 item 2). While set, THIS
		 * body's own queued movement input -- SRPC_PLAYER_POS and
		 * SRPC_PLAYER_STATE -- is dropped for the rest of the logical update,
		 * because a committed respawn replaced the body those samples were
		 * authored against. Armed and retired together with the server-wide
		 * barrier above, but READ per actor: the host's respawn latch must not
		 * swallow the guest's sneak/on-ground edge, and (once a guest respawn
		 * path exists) vice versa. That one-flag-for-two-bodies coupling is
		 * exactly what Astra item 2 rejected. */
		bool reject_pos_this_update;
		struct inventory inventory;
		struct inventory* active_inventory;
		/* A1b (D9 window binding): the world the open TILE window (chest,
		 * double chest, furnace, dispenser, sign) was opened in, stamped by
		 * that window's on_create from server_local_world_of(actor). Every
		 * backing resolution goes through server_local_world_from_handle, so
		 * a discarded / re-created world (stale generation) makes only this
		 * actor's window backing-less; nothing caches a row index or a world
		 * pointer. {0,0} = no tile window bound. */
		struct server_world_handle window_world;
		/* At-most-once selected-recipe transaction cache. Serial comparison
		 * rejects an older delayed ticket; an exact retry receives this result. */
		uint32_t recipe_craft_ticket;
		enum recipe_craft_reason recipe_craft_reason;
		short health;
		int oxygen;
		// Beta Entity.fire: >0 == on fire and burning down; <=0 == not lit (the
		// -PLAYER_FIRE_RESISTANCE baseline is the fire-block ignite ramp).
		int fire;
		/* Death-screen dwell (deviation #18 closure): the position of the
		 * lethal hit, captured at the alive->dead transition. Beta drops the
		 * inventory AT death (EntityPlayer.onDeath -> dropAllItems), but this
		 * engine's whole drop+journal+switch transaction runs later, at the
		 * SRPC_RESPAWN request -- the corpse may have drifted (client physics
		 * keep ticking the dead body, faithfully), so the transaction snaps
		 * back to this point to spawn the drops where Beta would have.
		 * has_death_pos=false (boot, world load) falls back to the live
		 * position: a quit-while-dead reload drops at the SAVED corpse
		 * position, the documented approximation. NOT persisted -- Beta
		 * persists nothing extra either (its drops already happened). */
		bool has_death_pos;
		double death_x, death_y, death_z;
		/* Beta EntityPlayer.playerSpawnCoordinate: optional bed HEAD coordinate,
		 * distinct from the permanent world SpawnXYZ below. */
		bool has_bed_spawn;
		int32_t bed_spawn_x, bed_spawn_y, bed_spawn_z;
		/* Beta EntityPlayer sleeping state (handoff P4(b)): entered by block_bed's
		 * night click, ticked by server_local_sleep_tick beside the world clock.
		 * sleep_timer counts to 100 (isPlayerFullyAsleep >= 100); only THEN does the
		 * night skip happen -- never instantly in the click handler. */
		bool sleeping;
		int sleep_timer;
		int32_t sleep_x, sleep_y, sleep_z; /* the bed HEAD being slept in */
		/* Permanent Data.SpawnXYZ world fallback. */
		int spawn_x, spawn_y, spawn_z;
		float vel_y, old_vel_y;
		/* Beta Entity.fallDistance: the continuous downward displacement already
		 * accumulated by moveEntity. Keep the scalar itself rather than deriving it
		 * from a start Y: upward piston/step movement leaves fallDistance unchanged,
		 * which a height-only representation cannot preserve across later samples. */
		float fall_distance;
		// Beta hurt-invulnerability window (i-frames): ticks remaining during
		// which an equal-or-weaker hit is absorbed. Decremented once per server
		// tick; set to PLAYER_HURT_RESIST_TICKS on a fresh hit. (Server-owned,
		// not synced from the client.)
		short hurt_resist_time;
		/* G8-BLAST-VELOCITY (ASTRA-RB06-RESIDUALS s2, ASTRA-V-REVIEW): Beta
		 * Entity.beenAttacked (velocityChanged). Set by EVERY committed,
		 * surviving FRESH hit -- melee, arrow, fall, fire, cactus, drowning,
		 * suffocation, explosion; null attacker included
		 * (EntityLiving.attackEntityFrom:293-335). Absorbed / excess-only
		 * hits leave it exactly as it was. Only the tracker phase
		 * (server_local_flush_actor_velocity, EntityTrackerEntry:125-127)
		 * consumes it: ONE Packet28-quantized absolute motion, then clear.
		 * No hit path sends velocity itself any more. `been_attacked_life`
		 * is the actor generation at set time: a replacement lifetime never
		 * receives it; a lethal commit clears it. Transient, never archived. */
		bool been_attacked;
		uint64_t been_attacked_life;
		/* Beta EntityPlayerMP.motionY: the SERVER's own vertical motion,
		 * persisted across ticks and advanced once per server update by
		 * server_local_tick_actor_knockback (EntityLiving.
		 * moveEntityWithHeading :435-516: collision clip, then
		 * (motionY - 0.08D) * (double)0.98F in air, liquid branches).
		 * Deliberately SEPARATE from vel_y/old_vel_y, which are the client
		 * fall-sample bookkeeping (Packet10Flying carries no motionY). Its
		 * X/Z partners are vel_x/vel_z (server-owned already). Written by
		 * knockBack and the blast impulse, read by knockBack and the
		 * tracker flush. Never moves the body (the client owns position). */
		double motion_y;
		/* next-gaps-0924 W1F: Beta Entity.ticksExisted (Entity.java:51,161),
		 * incremented once per living tick by server_local_tick_actor_regen
		 * and zeroed wherever a fresh EntityPlayer is built (respawn, world
		 * load). Its only reader is the Peaceful regen cadence
		 * (EntityPlayer.java:173). Runtime-only, like Beta (not in NBT). */
		int ticks_existed;
		/* next-gaps-0924/W2 P4b: Beta EntityLiving.deathTime for the push
		 * loop's canBePushed / own-loop window -- `++deathTime; if(deathTime
		 * > 20) setEntityDead()` [EntityLiving.java:155-159]: a dead player
		 * is still an EntityLiving for 20 ticks, then isDead. v5: advanced
		 * per packet in the network phase (server_local_actor_death_tick --
		 * EntityLiving.onUpdate runs from onUpdateEntity only), so the world
		 * pass reads the previous update's value, as on the dedicated
		 * server; zero while alive. Host and guest. Runtime-only. */
		int death_ticks;
		/* FAITHFUL SIMULATION -- [740c583: EntityPlayer.damageEntity]
		 * damageRemainder carries armor's /25 truncation between accepted hits.
		 * It is transient player state, like hurt_resist_time: never archived. */
		int damage_remainder;
		// Beta EntityLiving.field_9346_af: the STRONGEST hit magnitude taken in the
		// current i-frame window. A new hit during the window that EXCEEDS this deals
		// only the excess (incoming - last_damage) and bumps it, WITHOUT re-opening the
		// window or applying knockback (Beta var3=false). Refreshed on every fresh hit.
		short last_damage;
		// Beta EntityPlayerMP.motionX/Z: server-owned, never carried by
		// SRPC_PLAYER_POS. G8-BLAST-VELOCITY v3: DOUBLE, as Beta's motion is
		// (every knockBack / impulse / friction step accumulates in double;
		// float constants are widened at the operation exactly as Beta does).
		double vel_x, vel_z;
		/* v3: Entity.field_27012_bb (isInWeb), set by a server physics move's
		 * block sweep, consumed by the next one (Entity.moveEntity:224-232). */
		bool in_web;
		/* v3: accepted movement samples of this update awaiting the tracker
		 * phase's physics replay (server_local_actor_motion_enqueue); the
		 * generation they were recorded under. */
		struct actor_motion_event motion_q[ACTOR_MOTION_QUEUE];
		uint8_t motion_q_n; /* count, or ACTOR_MOTION_REPLAYING during replay */
		uint64_t motion_q_gen;
		/* V6 F1 round 3: how many queued events hold a movement credit, and
		 * that credit identity (svin_credit_*); released at replay, reset,
		 * stale drop or settle -- never on inbox dequeue. */
		uint8_t motion_credit_n;
		uint8_t motion_credit_slot;
		uint32_t motion_credit_epoch;
		uint64_t motion_credit_gen;
		// Monotonic nonzero sequence for authoritative position corrections (piston push,
		// knockback, teleport). Client movement samples must echo the exact current value.
		uint32_t correction_seq;
		/* residency-1005 (ASTRA-1005B-RULING.md:2-5, ASTRA-RESIDENCY-REVIEW
		 * items 1-7): pin, destination requests, recovery barrier and
		 * counters -- see struct rz_actor in residency.h. */
		struct rz_actor rz;
		// Beta Entity.distanceWalkedModified / nextStepDistance: the footstep
		// accumulator. distance_walked += sqrt(dx^2+dz^2)*0.6 per position update;
		// a "footstep" fires when it crosses next_step_distance (int, ++ each step)
		// with a solid block underfoot -- the hook for BlockFarmland trampling
		// AND (S2) the step sound (Beta Entity.moveEntity). next_step_distance
		// starts at 1.
		float distance_walked;
		int next_step_distance;
		// S2 splash latch: Beta Entity.inWater, the previous tick's body-in-water
		// result. random.splash fires on the false->true transition ONLY
		// [740c583: Entity.onEntityUpdate:205-232]. Initialized TRUE in create,
		// load AND respawn (the portal four-field precedent below) -- the
		// isFirstUpdate-equivalent guard, so loading/respawning INTO water never
		// plays a spurious splash.
		bool was_in_water;
		// ===== Nether-portal dwell (M5; Beta EntityPlayer.timeUntilPortal /
		// inPortal / timeInPortal, server-authoritative here) =====
		// time_until_portal: re-entry cooldown in ticks. Fresh player = 20 (Beta
		// field initializer); contact while nonzero REFRESHES it to 10; set 10 on
		// travel. time_in_portal: the 0->1 dwell ramp (+0.0125F per contact tick,
		// -0.05F decay -- exact float accumulation, ~80 ticks). in_portal: armed
		// by the contact scan, consumed by the next tick's state machine.
		// portal_travel_requested: threshold latch, consumed by the dimension
		// switch. All four MUST be initialized in BOTH server_local_create and
		// the world-load path (the struct is stack-allocated, never zeroed).
		int time_until_portal;
		float time_in_portal;
		bool in_portal;
		bool portal_travel_requested;
		/* S1-d: this body's own accepted-action tallies (see the
		 * enum server_player_action comment above this struct for exactly
		 * what each slot counts). The guest body is calloc'd, so it starts
		 * at zero by construction; s->player is part of an UNINITIALIZED
		 * automatic and is explicitly zeroed in server_local_init with the
		 * rest of this page. */
		uint32_t actions[SERVER_ACTION_COUNT];
	} player;
	/* S1-d: main-thread-readable MIRROR of the two owners' `actions` tallies
	 * -- [0] = s->player (host), [1] = s->guest_actor (guest). The heartbeat
	 * is composed on the MAIN thread, and s->guest_actor is a pointer this
	 * (server) thread frees on leave, so the emitter may not walk it; the
	 * increment helper publishes the acting body's fresh total here with a
	 * RELAXED atomic store instead, exactly like anchor_near_owed/
	 * anchor_oldest. Slot 1 is cleared when a NEW guest body binds, so a
	 * rejoin cannot make the newcomer inherit the previous occupant's tally. */
	uint32_t actor_actions[2][SERVER_ACTION_COUNT];
	/* Open wooden-chest views, all on this server thread. Items/cursors in
	 * each view remain private; shared storage changes are fanned out by key. */
	struct inventory* chest_views;
	/* Process-lifetime populate rollback base. It is reserved before the first
	 * world streams and borrowed by every Overworld/Nether instance, so a portal
	 * switch never re-enters the fragmented MEM2 allocator to acquire it. */
	struct server_world_populate_scratch* world_populate_scratch;
	/* A2-S: the ONE region-handle pool of this session (REGION_POOL_SLOTS
	 * across every world slot) -- see region_pool.h. Worlds bind to it at
	 * their first region acquisition (server_world_region_pool). */
	struct region_pool region_pool;
	/* A1a (split-dimensions-plan-0924, Astra D1/D8 + ruling astra-A1a-a):
	 * the session's world contexts. worlds[i].world_index == i always (set in
	 * server_local_init). Only worlds[0] is ever live today: single-player and
	 * split both run in slot 0 exactly as before, and slot 1 stays not-live
	 * (zeroed, generation 0) until A2 activates the second dimension.
	 * Reach a world through an explicit owner: a `w` you were given, the
	 * actor's committed world (server_local_world_of), a validated handle
	 * (server_local_world_from_handle), or -- only for a documented legacy /
	 * Overworld-only path -- &s->worlds[0].
	 *
	 * `world` is a DEPRECATED, FIXED alias of worlds[0] (same storage, never
	 * a pointer, never re-targeted) kept only for the code that used it before
	 * A1a. NEVER swap or copy another dimension into slot 0 to emulate a
	 * "current world". tests/test_world_alias_gate.c fails on any NEW use. */
	union {
		struct server_world worlds[SERVER_LOCAL_WORLD_SLOTS];
		struct server_world world; /* DEPRECATED alias of worlds[0] */
	};
	/* A2-T D4: the world the block callback being dispatched acts on
	 * (block_world, blocks.h). NULL = worlds[0], the only value single-player
	 * ever sees. Set only for the extent of one dispatch by
	 * block_world_enter/leave -- never a "current world" swapped into slot
	 * 0. Server thread only. */
	struct server_world* block_world;
	dict_entity_t entities;
	/* IMP-TE-CAPS: every tile table below is a growable MEM2 slab owned by
	 * te_grow (server_local_te_tables_init); te_rows[kind] is its current
	 * row count (>= the MAX_* initial count). Index rows only below that. */
	struct complex_block_pos* chest_pos;
	struct item_data (*chest_items)[MAX_CHEST_SLOTS];
	struct complex_block_pos* sign_pos;
	char (*sign_texts)[SIGN_SIZE];
	// Timed-furnace tile entities (M4), keyed by position like chests: furnace_pos[i].y
	// < 0 == free slot. furnaces[i] holds the burn/cook state + the 3 ItemStacks
	// (input/fuel/output); it is the authoritative store the per-tick furnace_tick
	// mutates and the open furnace window live-binds to (server_furnace.c).
	struct complex_block_pos* furnace_pos;
	struct furnace_te* furnaces;
	// Dispenser tile entities (dispenser-slice), keyed by position like
	// furnaces: dispenser_pos[i].y < 0 == free slot. dispensers[i] holds the
	// 9-slot inventory + the TE's own dispenserRandom (server_dispenser.c).
	struct complex_block_pos* dispenser_pos;
	struct dispenser_te* dispensers;
	// Note-block tile entities (noteblock-slice), keyed by position like
	// dispensers: note_pos[i].y < 0 == free slot. notes[i] holds the
	// persisted 0..24 pitch value. note_prev_powered[i] is a LIVE-ONLY
	// sidecar (Beta TileEntityNote.previousRedstoneState -- never persisted;
	// server_noteblock.c).
	struct complex_block_pos* note_pos;
	int8_t* notes;
	bool* note_prev_powered;
	// Jukebox tile entities (jukebox-slice), keyed by position like
	// dispensers: record_player_pos[i].y < 0 == free slot. records[i] holds
	// the inserted record's item id (0 = no record, server_jukebox.c).
	struct complex_block_pos* record_player_pos;
	int32_t* records;
	// FH-17: MobSpawner tile entities, keyed by position like dispensers/
	// jukeboxes: spawner_te_pos[i].y < 0 == free slot. spawner_tes[i] holds
	// the persisted EntityId string + Delay (server_spawner.c). This is the
	// resident-chunk-lifetime store; active_spawners[] below is the separate,
	// smaller near-player refire-timer tracker that reads mob type FROM here
	// (server_local_tick_spawners) instead of the block's metadata nibble.
	struct complex_block_pos* spawner_te_pos;
	struct spawner_te* spawner_tes;
	/* A1b: owner keys for the seven tables above (one shared copy). */
	struct te_owner_table te_owners;
	/* A2-S: restore reservations for the two flat-archive kinds (see
	 * te_store.reserved). Zero = unreserved. */
	uint8_t* chest_reserved;
	uint8_t* sign_reserved;
	/* IMP-TE-CAPS: current row count per kind (enum te_kind order) and the
	 * slab owner every production view grows through. */
	int te_rows[TE_KIND_FIXED_COUNT];
	struct te_grow_ctx te_grow;
	// In-flight piston moving blocks (Beta TileEntityPiston). Dynamic sparse
	// ownership removes the reachable five-piston/65-entry shortcut while keeping
	// stable indices through re-entrant settle notifications. Each entry carries
	// both progress floats and a monotonic loaded-list order.
	struct piston_moving_store piston_moving;
	struct active_spawner active_spawners[MAX_ACTIVE_SPAWNERS];
	// Resident map-data cache (see MAX_MAP_CACHE above): map_cache_id[i] == INT32_MIN
	// is a free slot. map_cache_dirty[i] tracks whether map_cache_data[i]
	// has scan/creation changes not yet flushed to "data/map_<id>.dat" --
	// flushed at the same checkpoint/autosave boundaries chests/signs/
	// furnaces already use (server_local_checkpoint_legacy_tile_entities,
	// the autosave interval), plus opportunistically evicted (flush-before-
	// reuse) by item_map.c's cache lookup. map_cache_touch is a monotonic
	// per-slot "last used" stamp; map_cache_touch_counter is the shared
	// clock those stamps compare against (LRU = smallest touch value).
	int32_t map_cache_id[MAX_MAP_CACHE];
	struct map_data map_cache_data[MAX_MAP_CACHE];
	bool map_cache_dirty[MAX_MAP_CACHE];
	uint32_t map_cache_touch[MAX_MAP_CACHE];
	uint32_t map_cache_touch_counter;
	/* MapData.func_28169_a's first-updater list, per resident map. Slot 1
	 * additionally carries its admission generation, never a borrowed actor. */
	uint8_t map_cache_holder_count[MAX_MAP_CACHE];
	uint8_t map_cache_holders[MAX_MAP_CACHE][2];
	uint64_t map_cache_holder_generation[MAX_MAP_CACHE][2];
	/* Held-map render slice (2026-08-26): per-player CRPC_MAP_DATA gate
	 * (0 host, 1 guest). The last colour payload delivered, compared by memcmp
	 * each held-map tick so a send happens only on real change (or map-id
	 * switch) -- no coupling to map_cache_dirty (that flag belongs to disk
	 * persistence and is cleared by the save path on its own schedule).
	 * map_client_sent_any[player] == false => nothing sent yet (reset at the
	 * session map-cache boundary, also safe for zero-init fixtures). Id 0 is a
	 * VALID map id, so a plain "sent id" sentinel would wrongly suppress
	 * map 0's first send). A guest admission generation forces a fresh sync. */
	bool map_client_sent_any[2];
	int32_t map_client_sent_id[2];
	uint8_t map_client_shadow[2][MAP_DATA_COLORS_SIZE];
	uint64_t map_client_generation[2];
	uint64_t world_time;
	// Beta 1.7.3 weather (WorldInfo + World.updateWeather/updateBlocksAndPlay-
	// CaveSounds [740c583]). raining/thundering + their timers persist to
	// level.dat (LEVEL_WEATHER_* in level_archive.h); the *_strength pair is
	// RUNTIME ONLY (Beta never persists it -- a fresh load ramps back in over
	// ~100 ticks like a fresh Beta world). weather_pos_lcg is Beta's persistent
	// per-world `field_9437_g` int (f = f*3 + 1013904223), advanced ONLY by the
	// cave-sound/lightning/snow-freeze branches server_weather_effects_tick
	// ports -- kept separate from the engine's existing general random-tick RNG
	// (server_world_random_tick's rand_src draws) by design; see
	// WEATHER_SLICE_NOTES.md for why the two are not merged. pos_lcg/
	// sound_counter are SEEDED at world create/load (server_local.c) from
	// s->rand_src, matching Beta's World constructor
	// (`field_9437_g = new Random().nextInt()`, `soundCounter =
	// this.rand.nextInt(12000)`) -- NOT persisted, exactly like Beta.
	struct {
		bool raining;
		bool thundering;
		// debug.weather_force ("rain"/"thunder"/"off"): DEBUG-ONLY lever set
		// by main.c before server start, applied once in server_weather_seed
		// (forces the initial state + a long timer so field/capture testing
		// can see weather on demand -- field gap 2026-08-31: SP weather was
		// unverifiable by patience). "off" (default) touches NOTHING -- the
		// sim stays byte-identical, same class as debug.superflat.
		uint8_t debug_force; // 0=off 1=rain 2=thunder
		int32_t rain_time;
		int32_t thunder_time;
		float rain_strength;
		float prev_rain_strength;
		float thunder_strength;
		float prev_thunder_strength;
		// Beta World.field_27168_F: ticks left on the "a bolt just struck"
		// flash cue, set to 2 when a lightning bolt spawns, decremented every
		// updateWeather tick. Render/telemetry only -- no gameplay effect.
		int32_t flash_ticks;
		uint32_t pos_lcg;
		// Beta World.soundCounter: ticks until the next cave-sound roll is
		// eligible; ALSO gates whether the shared position LCG advances this
		// tick for the cave-sound branch (see the citation in
		// server_weather.c). The sound itself is a documented no-op (S0's
		// SOUND_AMBIENT_CAVE is already driven client-side by its own mood
		// counter, client_display_tick.c).
		int32_t sound_counter;
		// Beta EntityLightningBolt's own multi-tick lifecycle (lightningState
		// 2->1->0->negative->restrike-or-die, boltLivingTime), ticked once per
		// bolt per server tick by server_weather_tick_lightning_bolts
		// (server_weather.c) [740c583: EntityLightningBolt.onUpdate]. A
		// realloc-grown array -- NOT a fixed cap (house rule: no silent caps;
		// concurrent bolts are rare and short-lived, but an artificial ceiling
		// would silently drop a real strike's ongoing restrikes/damage).
		struct active_lightning_bolt* active_bolts;
		size_t active_bolts_count;
		size_t active_bolts_cap;
	} weather;
	/* next-gaps-0924/w2: Beta World.skylightSubtracted -- a per-tick CACHED world field
	 * (World.tick:1757-1759; World.calculateInitialSkylight at every world construction).
	 * Written only by server_local_skylight_recompute; read through
	 * server_local_skylight_subtracted / server_local_is_daytime. skylight_cached false (a
	 * memset fixture that never loaded or ticked) reads the clock's value live. */
	int skylight_subtracted;
	bool skylight_cached;
	// FIX #5: CRPC_WEATHER_SET change-gate. The last weather state actually
	// PUBLISHED to the client (distinct from the `weather` struct above,
	// which is the live simulation value) -- server_local_update resends only
	// when one of these four fields differs from the live value.
	// weather_last_sent_valid starts/resets false so the very first tick of a
	// fresh session or a freshly loaded world always sends once regardless
	// (world load / client attach must never rely on a stale snapshot from a
	// previous world). Updated by server_local_weather_mark_sent, called
	// right after EVERY actual CRPC_WEATHER_SET send (both the main per-tick
	// broadcast and server_local_wake_up's own immediate same-tick resend) so
	// the gate stays accurate regardless of which call site last published.
	bool weather_last_sent_valid;
	bool weather_last_sent_raining;
	bool weather_last_sent_thundering;
	float weather_last_sent_rain_strength;
	float weather_last_sent_thunder_strength;
	string_t level_name;
	struct level_archive level;
	bool paused;
	/* Clean-quit plumbing (the amahpour-shared shutdown race): the dedicated
	 * server thread used to run while(1) with its handle discarded on a stack
	 * local, so main() could only exit by ABANDONING it -- potentially
	 * mid-region-write (on Wii, a save-corruption window on exit-to-loader).
	 * server_local_start stores the handle and arms thread_started;
	 * server_local_shutdown raises thread_stop, joins, and disarms. The thread
	 * re-checks the flag once per tick (<= ~50ms sleep + one tick of work), so
	 * shutdown latency is bounded. Fields live on main()'s UNZEROED stack
	 * struct -- server_local_init must initialize them explicitly (the GB-01
	 * weather-bolt lesson; guarded by the poison test). */
	struct thread thread;
	/* CR-08 (2026-08-20): plain `bool`, NOT `volatile` -- volatile gives ISO C
	 * neither atomicity nor cross-thread ordering, so it never actually
	 * synchronized this flag, it only discouraged the compiler from caching it
	 * (accidentally, and not reliably under LTO). Every read/write of
	 * thread_stop and thread_done goes through a GCC __atomic builtin at the
	 * call site (server_local.c: the shutdown writer, the server thread's loop
	 * read, and the two pre-thread-start resets) -- __ATOMIC_RELEASE on the
	 * writer, __ATOMIC_ACQUIRE on the cross-thread reader. These are C99-mode
	 * GCC extensions (no <stdatomic.h>/C11 needed) and lower to plain aligned
	 * lwz/stw on PPC32 and x86 alike, so there is no libatomic pull-in and no
	 * Wii-toolchain risk. */
	bool thread_stop;
	/* Set by the server thread as its LAST act (2026-08-20 provider-swap
	 * redesign, hostile-review finding 1): server_local_shutdown must not
	 * blind-join, because the server thread may be blocked inside
	 * clin_rpc_send's token backpressure -- whose only refiller,
	 * clin_update, runs on the very (main) thread doing the join. Shutdown
	 * spins clin_update()+msleep until this flips, THEN joins (which
	 * returns immediately): the waiter drains the mailbox the waited-on
	 * thread is stuck on. CR-08 (2026-08-20): the REAL ordering barrier for
	 * everything the server thread did before this flip is still the
	 * thread_join below it, not this flag -- that has not changed. What the
	 * accessors add is a second, narrower guarantee this field needs on its
	 * own account: the flag's own value must actually cross from the server
	 * thread to the main thread's polling read (not sit cached, not tear).
	 * Plain `bool` (not `volatile`); every access goes through a GCC __atomic
	 * builtin at the call site, __ATOMIC_RELEASE on the server thread's
	 * last-act write, __ATOMIC_ACQUIRE on the main thread's drain-loop read. */
	bool thread_done;
	bool thread_started;
	ptime_t last_tick;
	// Debug: when set (config "debug.give": "on"), the player's hotbar is
	// filled with a fixed test kit after ordinary loads (including a death
	// reload), for hands-on mechanic testing. A live portal destination/rollback
	// preserves the same player's exact inventory instead. Read once on the main
	// thread in server_local_create, so the server thread reads it race-free.
	bool give_test_kit;
	// One-shot load-reason latch consumed by the async inventory finalizer.
	// Portal destination/rollback loads keep the same live inventory object;
	// ordinary/death loads reconstruct it from level.dat.
	bool preserve_live_inventory_on_finish;
	bool inventory_finish_pending;
	// FIX #4: gates the per-tick server_local_retry_orphaned_container_drops
	// sweep (MAX_CHESTS + MAX_FURNACES linear scan, every tick, unconditionally
	// before this fix). server_local_spawn_destroyed_container_items arms this
	// whenever it returns false -- exactly the "row retained after a failed
	// spill" moment block_chest.c/block_furnace.c/block_iron_chest.c's onBreak
	// handlers already comment as "retain the row for the server retry pass" --
	// so the sweep only runs a tick after there is actually something for it to
	// do. The sweep itself clears this once a full pass finds nothing left
	// pending (see server_local_retry_orphaned_container_drops).
	bool orphan_container_retry_pending;
	// Debug ("debug.chest_selftest"): once, right after the world finishes
	// loading, run the per-chunk chest-persistence lifecycle end to end on a
	// throwaway far chunk and write PASS/FAIL to chest_selftest_result.txt --
	// verifies the chest-store redesign on real hardware with no player input.
	bool chest_selftest;
	// Debug ("debug.fluid_demo"): once at world load, build a small no-input
	// water/lava showcase ringed around spawn so the Beta fluid behaviour can be
	// screenshotted on real Wii / Dolphin (where the emulated Wiimote can't be
	// driven). Like chest_selftest, read once on the main thread.
	bool fluid_demo;
	// Debug ("debug.furnace_demo"): once at world load, place a lit, mid-smelt
	// furnace west of spawn and tilt the camera onto it, so the M4 furnace GUI
	// flame + cook arrow can be screenshotted on hardware/Dolphin (right-click to
	// open). Set from main.c after create, like pig_demo/spider_demo.
	bool furnace_demo;
	// Debug ("debug.map_demo", held-map capture rig 2026-08-26): once, ~5s
	// after load, give a fresh ITEM_MAP to hotbar slot 0 and select it so
	// the first-person live-map render is capturable with zero input.
	// (Re-applied from reverted 37385a8 for the breaker-#2 investigation;
	// post-split these live in the state header.)
	bool map_demo;
	int map_demo_defer;
	/* The demo furnace is built before the final input epoch, but its GUI cannot
	 * open while the client intentionally owns screen_load_world. Open it only
	 * after the exact ACK completes the handoff. */
	bool furnace_demo_open_pending;
	w_coord_t furnace_demo_open_x;
	w_coord_t furnace_demo_open_y;
	w_coord_t furnace_demo_open_z;
	// Debug ("debug.redstone_demo"): once at world load, build a redstone NOT-gate
	// (lit source torch -> wire run -> output torch off, + a lit control torch) on a
	// cleared pad west of spawn, to confirm the M4 wire flood + torch inverter + the
	// R3 two-hop arming on Dolphin/console with no input. Set from main.c after create.
	bool redstone_demo;
	// Debug ("debug.piston_demo"): a few seconds after world load (see piston_demo_defer), build a
	// small camera-facing piston showcase (regular + sticky, powered/extended + unpowered/plate)
	// straight ahead of spawn at eye height, so the M4 piston render is screenshottable on
	// Dolphin/console with no input. Set from main.c after create.
	bool piston_demo;
	// Debug ("debug.piston_anim"): a TIMED extend/retract showcase (regular + sticky piston, built
	// UNPOWERED via the piston_demo deferred path, then the server toggles their power on a slow loop).
	// piston_anim_tick is the per-tick phase counter (0 = not running); piston_anim_b{x,y,z} captures the
	// resolved player base at build so the per-tick driver can recompute the fixture coords. Lets a
	// screenshot burst catch placed -> extended -> retracted (the sticky pull vs the regular stranding).
	bool piston_anim;
	int piston_anim_tick;
	int piston_anim_bx, piston_anim_by, piston_anim_bz;
	// Debug ("debug.piston_blast_demo"): the D1+D3 explosion-vs-piston PROOF rig (bug-hunt
	// 2026-07-17). Builds ONE up-facing piston with a PLANKS payload (drop-unique in the scene) via
	// the piston_demo deferred path, UNPOWERED; piston_blast_drive (per server tick) powers it after
	// a settle window, detonates the mid-move id-36 planks carrier, and writes the verdict to
	// piston_blast_result.txt on the SD root (chest_selftest_result.txt precedent) -- D1: the carve
	// must force-finish the moving TE (no ghost re-materialization at settle); D3: the stored planks
	// must drop UNCONDITIONALLY. piston_blast_tick is the per-tick phase counter (0 = not running);
	// piston_blast_b{x,y,z} captures the resolved player base at build. The remaining fields latch
	// the blast-tick evidence (pre/post carve ids + the no-TE-at-carved probe) so the deferred
	// verdict write can compare against the immediate post-carve state.
	bool piston_blast_demo;
	int piston_blast_tick;
	int piston_blast_bx, piston_blast_by, piston_blast_bz;
	uint8_t piston_blast_pre_head, piston_blast_post_head;		 // (py+1) id before/after the carve
	uint8_t piston_blast_pre_carrier, piston_blast_post_carrier; // (py+2) id before/after the carve
	bool piston_blast_head_carved, piston_blast_center_carved;
	bool piston_blast_no_te_ok; // no moving-TE tracked at any carved cell right after the carve
	// ===== Headless Dolphin/console PROOF demos for the 2026-07-27 maintainer hardware
	// report (unconfirmed on real hardware: saved-chunk reload, DIM-1 chunk loading, TNT
	// chain reactions, sticky pistons). Each follows the piston_blast_demo template: a
	// self-asserting rig built via the piston_demo deferred path, driven per server tick
	// next to piston_blast_drive, graded into its OWN SD-root verdict file whose final
	// line is "VERDICT PASS|FAIL|SKIP" (tools/dolphin_prove.ps1 -VerdictFile <name>).
	// ONLY ONE of the demos sharing the piston_demo build path (piston_demo, piston_anim,
	// piston_blast_demo, nether_roundtrip_demo, tnt_chain_demo, piston_sticky_demo) may
	// be armed per run -- server_local_piston_demo dispatches to exactly one branch, and
	// the rigs' Z offsets (round-trip markers pz+4, sticky pz+5, TNT line pz+10) assume
	// no co-tenant scene. =====
	// Debug ("debug.nether_roundtrip_demo"): the saved-chunk + DIM-1 round-trip PROOF ->
	// nether_roundtrip_result.txt. Builds byte-exact marker blocks near the player in the
	// Overworld, then portal-travels Over->Nether->Over->Nether through the REAL dwell +
	// server_local_portal_travel machinery (a lit pane built around the aim-locked player
	// with the dwell zeroed -- the portal_demo precedent), building DIM-1 markers on the
	// first Nether visit. The heart: the Overworld markers are re-checked (id AND
	// metadata) after their chunks were saved on the leg-1 exit and REGION-RELOADED for
	// the leg-2 return, and the DIM-1 markers after theirs on the leg-3 re-entry. Every
	// wait phase polls with a bounded live-tick timeout -> VERDICT FAIL, never a hang.
	bool nether_roundtrip_demo;
	// Stage machine: 0 = not built; 1..7 = running (timeline at nether_roundtrip_drive);
	// -1 = verdict written. The non-zero stage also blocks the load-boundary defer
	// re-arm that fires after EVERY portal arrival, so the rig builds exactly once.
	// nether_roundtrip_tick counts live server ticks spent in the CURRENT stage (it does
	// not advance during a world switch, so the wait timeouts bound the live polling; a
	// wedge inside the switch itself never reaches the drive and is caught by the
	// prover's wall-clock timeout instead).
	int nether_roundtrip_stage;
	int nether_roundtrip_tick;
	int nether_roundtrip_mx, nether_roundtrip_my, nether_roundtrip_mz; // Overworld marker base
	int nether_roundtrip_nx, nether_roundtrip_ny, nether_roundtrip_nz; // DIM-1 marker base
	bool nether_roundtrip_ow_ok; // Overworld markers byte-exact after the leg-2 reload
	char nether_roundtrip_ow_detail[96]; // first-mismatch detail latched at that check
	// Debug ("debug.tnt_chain_demo"): the TNT chain-reaction PROOF -> tnt_chain_result.txt.
	// Builds a stone carve pad + a line of SIX TNT spaced 2 apart (each power-4 blast
	// trivially reaches its neighbour), ignites exactly ONE through the real prime path
	// (server_local_prime_tnt_block, Beta 80-tick fuse) so every other detonation must
	// come from the Beta chain rule, then after a bounded window (well past 6 fuses plus
	// the FCAVEX_PROFILE_WII_LOW 2-blast/tick lossless deferral) asserts: no BLOCK_TNT
	// left in the rig box, no ENTITY_PRIMED_TNT stranded in the entity dict (a lost
	// deferred explosion leaves one), and the pad actually carved (detonations ran).
	// tnt_chain_tick: 0 = idle, >0 = the per-tick phase counter, -1 = verdict written
	// (also blocks the defer re-arm after a reload). b{x,y,z} = resolved player base.
	bool tnt_chain_demo;
	int tnt_chain_tick;
	// Debug ("debug.mob_motion_rig", hw_run profile mob_motion_2m; lane C mob
	// motion units S2 proof): builds the three-check mob motion fixture (TNT
	// lift tube, zombie step trench, pig free fall) through the deferred demo
	// path; the rig state and its verdict live file-static in server_local.c
	// (s_mm_rig / s_mm_evidence), published to the heartbeat.
	bool mob_motion_rig;
	int tnt_chain_bx, tnt_chain_by, tnt_chain_bz;
	// Debug ("debug.piston_sticky_demo"): the sticky-piston retraction PROOF ->
	// piston_sticky_result.txt. ONE up-facing STICKY piston with a wool payload; the
	// driver powers it with a torch (the piston_anim toggle mechanism), latches the
	// extended evidence after the T+2 settle (base meta 1|8, head 34 at py+1, wool
	// pushed to py+2), unpowers it, then asserts the Beta sticky PULL: wool back at
	// py+1, head gone, base meta unpowered, and the moving store empty at the rig cells.
	// piston_sticky_tick: 0 = idle, >0 = running, -1 = verdict written (blocks the
	// defer re-arm). The ext_* fields latch the observed extended state for the file
	// (0xFF = the cell read unloaded).
	bool piston_sticky_demo;
	int piston_sticky_tick;
	int piston_sticky_bx, piston_sticky_by, piston_sticky_bz;
	bool piston_sticky_ext_ok;
	uint8_t piston_sticky_ext_bm;						// observed powered base meta (want 9)
	uint8_t piston_sticky_ext_t1, piston_sticky_ext_m1; // observed (py+1) id:meta (want 34:9)
	uint8_t piston_sticky_ext_t2, piston_sticky_ext_m2; // observed (py+2) id:meta (want 35:0)
	// Debug ("debug.world_reload_demo" = "create"|"verify"): the COLD-BOOT world reload
	// PROOF (maintainer 2026-07-27: "loading already-saved worlds is not confirmed").
	// CREATE (boot 1, autoload "+new"): build the roundtrip marker set + drop one planks
	// item at the anchor, run a full live-world checkpoint, write phase_create to
	// world_reload_result.txt. VERIFY (boot 2, autoload "1", the prover syncs saves/ out
	// of WiiSD.raw between boots): byte-exact marker re-check + the planks item entity
	// restored from the cold entity archive, verdict to the same file. Anchor =
	// floor(player) + 4 on Z at drive time -- the player position is part of the saved
	// state, so both boots resolve the identical anchor. world_reload_tick: 0 = idle,
	// >0 = running, -1 = verdict written (blocks the load-boundary defer re-arm).
	bool world_reload_create;
	bool world_reload_verify;
	int world_reload_tick;
	int world_reload_bx, world_reload_by, world_reload_bz;
	// Debug ("debug.redstone_drop"): with redstone_demo, a TIMED support-drop showcase -- a centered
	// lit redstone torch on a stone mount whose mount the server REMOVES then re-adds on a slow loop,
	// so a capture catches the torch DROP (BlockRedstoneTorch.onNeighborBlockChange support-drop) and
	// its reappear. redstone_drop_tick is the per-tick phase counter (0 = not running); the b{x,y,z}
	// capture the resolved subject cell (the mount block under the torch).
	bool redstone_drop_anim;
	int redstone_drop_tick;
	int redstone_drop_bx, redstone_drop_by, redstone_drop_bz;
	// Debug ("debug.redstone_clock_stress"): hardware proof profile that builds a
	// small forced redstone inverter clock and keeps toggling it through the real
	// server_world_set_block -> notify -> scheduled-tick path for the full run.
	bool redstone_clock_stress;
	int redstone_clock_tick;
	int redstone_clock_bx, redstone_clock_by, redstone_clock_bz;
	int redstone_clock_defer;
	// Debug ("debug.redstone_cascade_stress" / int "debug.redstone_cascade_stress_int"=1 via
	// --cw-set): next-gaps-0924/G5 oversized-cascade fixture for perf_stress_5m -- a 361-wire field
	// toggled every second + a 40-stage floating-torch chain cascade every minute.
	bool redstone_cascade_stress;
	int redstone_cascade_tick;
	int redstone_cascade_bx, redstone_cascade_by, redstone_cascade_bz;
	int redstone_cascade_chain_x, redstone_cascade_chain_z;
	int redstone_cascade_defer;
	// Debug ("debug.sleep_spawn_stress" / int "debug.sleep_spawn_stress_int"=1 via --cw-set):
	// next-gaps-0924/w5 sleep-engaged fixture for perf_stress_5m -- an unlit open field with a bed;
	// the host is put back to bed at night every SLEEP_SPAWN_STRESS_PERIOD ticks so the completing
	// tick (performSleepSpawning, up to 20 candidates / 16+ A* searches) lands in the worst-tick
	// instruments. sleep_stress_cycles / _spawns count completed sleeps / sleep spawns.
	bool sleep_spawn_stress;
	int sleep_stress_tick, sleep_stress_defer;
	// Debug ("debug.light_queue_stress" / int "debug.light_queue_stress_int"=1
	// via --cw-set): G9 item 6 engaged pending-light workload for the
	// hardware profiles (perf_stress_5m + collector --require-g9-evidence).
	bool light_queue_stress;
	int light_queue_tick, light_queue_defer;
	int light_queue_bx, light_queue_by, light_queue_bz;
	int sleep_stress_bx, sleep_stress_by, sleep_stress_bz;
	int sleep_stress_cycles, sleep_stress_spawns;
	uint64_t sleep_stress_spawn_pending_time;
	// save_migrate real-Wii profile: build an old-format fixture in N distinct
	// regions, close it, reopen it through server_local_load_world_impl, then
	// validate all four physical wall orientations after normal streaming.
	// Per region the fixture also plants one legacy iron-chest block (bare id
	// 95, no TE -- converted because the fixture world is cavewii_created) and
	// one legacy metadata-encoded dungeon spawner block (no TE -- always
	// synthesized), so the SAME production reopen exercises all three
	// CaveWiiSaveVersion steps (2 signs / 3 chests / 4 spawners), not just the
	// first. save_migrate_chests_verified/save_migrate_spawners_verified mirror
	// save_migrate_verified's per-region physical-recheck counter for those two
	// new fixture kinds.
	bool save_migrate_profile;
	int save_migrate_regions;
	int save_migrate_stage;
	int save_migrate_region_index;
	int save_migrate_defer;
	int save_migrate_verified;
	int save_migrate_chests_verified;
	int save_migrate_spawners_verified;
	// Debug ("debug.sign_demo"): wave-3 in-world sign TEXT evidence -- places
	// ONE wall sign ahead of the player carrying known four-line text and
	// faces the player at it, so the text pass can be screenshotted with NO
	// input (typing needs the OSK, which no headless/emulated rig can drive).
	// Seeded through the same sign_pos/sign_texts slot + the same
	// CRPC_SIGN_TEXT the edit echo sends, so the renderer sees an ordinary
	// sign. Deferred like its siblings so the seed CRPC clears the load burst.
	bool sign_demo;
	int sign_demo_defer;
	// Debug ("debug.painting_demo"): PROJECT_MEMORY 2026-09-01 deferred item
	// #1 visual-proof rig -- build a small stone wall a few blocks ahead of
	// the idle player and hang ONE painting on it through the SAME
	// production path a real ItemPainting placement uses
	// (server_local_spawn_painting -> entity_painting()), so the bundled CC0
	// assets/art/kz.png atlas (tools/gen_kz_placeholder.py) can be
	// screenshotted rendering with NO player input (item-use aiming needs a
	// mouse/IR pointer no headless/emulated capture rig can drive). Deferred
	// like sign_demo so the wall's CRPC_SET_BLOCK stream + the spawn's
	// CRPC_SPAWN_PAINTING clear the load burst.
	bool painting_demo;
	int painting_demo_defer;
	// Debug ("debug.menu_dwell_sign_demo"): the menus_dwell hardware profile's
	// sign-OSK evidence -- once at world load, clear a small pocket and place
	// ONE wall-mounted sign (full-face selection box, not the standing
	// variant's thin floor slab -- far more reliable for a FIXED-angle
	// autopilot aim to land on) at the SAME offset/height convention
	// redstone_clock_stress already uses, registered in sign_pos/sign_texts
	// exactly like block_sign.c's onItemPlace (see server_local.c). The
	// EXISTING debug.autopilot "aim" + one-shot place pulse (armed
	// separately, see hw_run.h's autopilot_place) is what actually opens it
	// -- this flag only builds the block to click.
	bool menu_dwell_sign_demo;
	int menu_dwell_sign_defer;
	// CREATIVE MODE -- a NET-NEW ADDITION on top of the Beta 1.7.3 base,
	// sanctioned by explicit maintainer exception 2026-08-25
	// (docs/creative_mode.md; Beta 1.7.3 itself has no creative mode).
	// Phase 1: player invulnerable (server_local_set_player_health gate),
	// no item consumption on place/use/shoot/throw, instant client dig.
	// PER-WORLD since 2026-09-01 (maintainer decision 1): set from the loaded
	// world's LEVEL_GAME_MODE tag in server_local_load_world_impl, on EVERY
	// load, so it can never leak across worlds. The retired
	// "gameplay.creative" config no longer touches it. Absent tag = survival.
	// World RULES are untouched either way.
	bool creative_mode;
	// Debug ("debug.water_demo"): once at world load, seal the spawn inside a
	// glass water tank so the player is submerged with no input -- used to show
	// the Beta air-bubble HUD on hardware/Dolphin. Read once on the main thread.
	bool water_demo;
	// Debug ("debug.zombie_demo"): once at world load, clear a small flat arena
	// at spawn and place a zombie one block from the idle player, so the new
	// Beta mob->player melee damage + knockback can be screenshotted with no
	// input on hardware/Dolphin. Read once on the main thread.
	bool zombie_demo;
	// Debug ("debug.pig_demo"): place a PIG a few blocks ahead with the camera tracking
	// it -- a no-input render check for the passive-animal model. Set on s after create
	// (main.c), like far_lod. Run with debug.superflat for a clean stage.
	bool pig_demo;
	// Debug ("debug.spider_demo"): place a SPIDER a few blocks ahead with the camera
	// tracking it -- a no-input render check for the post-audit spider model. Set in main.c.
	bool spider_demo;
	// Debug ("debug.render_mob" = MONSTER_* id, -1 = off): generic per-mob render check --
	// spawns that mob on a stone platform with the camera tracking it. Set in main.c.
	int render_demo_mob;
	// Countdown (ticks) until the render demo STAGES. The demo used to run inside
	// the world-load handler and its one-shot CRPC_PLAYER_POS teleport did not
	// take (camera left aiming from the loading position at fragment-sized mobs);
	// arming a ~5s defer fixed it, so the defer stays. CORRECTION (2026-06-11
	// channel audit): the original "chunk-send burst overflows the clin queue and
	// DROPS the teleport" attribution is wrong -- clin_rpc_send blocks on a
	// token-conserved pool (the inbox can never be full at send time, either
	// platform; see clin_rpc_send), and the cited evidence "rpcdrop>0" is
	// svin_dropped, which counts the OTHER direction (client->server
	// SRPC_PLAYER_POS drops, routine during load). Same for the once-queued "clin
	// can drop CRPC_SPAWN_MONSTER, port the svin reliable-ring" follow-up from
	// the M2(8c) commit: refuted, closed -- the only real client-side spawn-drop
	// path is the entity-alloc OOM in clin_process (loud printf). Whatever lost
	// the inline teleport, it was not channel overflow; the defer also lets the
	// stage chunks install before capture, which it needs anyway. Debug-only
	// staging; 0 = idle.
	int render_demo_defer;
	// Countdown (ticks) until the redstone_demo BUILDS (mirrors piston_demo_defer below). The build is
	// staged ~5s after load, not inline: its per-block CRPC_SET_BLOCK edits share the clin queue with the
	// world-load chunk flood, so an inline build overflows the queue and the client never renders the
	// circuit. Deferring also lets it build at the RESOLVED s->player (centered ahead) instead of the
	// level-archive spawn. Framing is handled by debug.autopilot=aim, so no camera CRPC is sent. 0 = idle.
	int redstone_demo_defer;
	// Countdown (ticks) until the piston_demo BUILDS. Like render_demo_defer, the build is staged
	// ~5s after load instead of inline: its per-block CRPC_SET_BLOCK edits share the clin queue with
	// the world-load chunk flood, so an inline build overflows the queue and the client never renders
	// the pistons (only natural terrain). Deferring lets the flood drain first. Set when piston_demo
	// is armed at finished_loading; 0 = idle.
	int piston_demo_defer;
	// Debug ("debug.ice_demo"): once at world load, build a broadside wall of ICE a few
	// blocks ahead of spawn (stone backdrop) and frame it with debug.autopilot=aim, so the
	// authentic translucent Beta ice (anim.png pass-1) is screenshottable with no input --
	// the in-game proof that make_beta_pack.ps1's anim.png ice composite reaches the renderer
	// (ice is otherwise the one block a Beta pack can't override; see atlas_parity_audit.md).
	// Set from main.c after create. Deferred via ice_demo_defer (mirrors piston/redstone).
	bool ice_demo;
	// Countdown (ticks) until the ice_demo BUILDS. Like piston_demo_defer, staged ~5s after
	// load (not inline) so its per-block CRPC_SET_BLOCK edits survive the world-load clin
	// overflow and the wall builds at the resolved s->player. 0 = idle.
	int ice_demo_defer;
	// Debug ("debug.farmland_demo"): once at world load, lay a field of hydrated farmland + mature
	// wheat around the player so an autopilot WALK leaves a visible trail of trampled DIRT (crops
	// pop) -- the in-game proof of BlockFarmland.onEntityWalking. Set from main.c after create;
	// deferred via farmland_demo_defer (mirrors ice/piston). 0 = idle.
	bool farmland_demo;
	int farmland_demo_defer;
	// Debug ("debug.fire_demo"): once at world load, build a wood structure with fire at its base
	// straight ahead of the player (aim-framed), so the rewritten Beta fire spread + the new animated
	// flame are screenshottable with no input. Set from main.c after create; deferred via
	// fire_demo_defer (mirrors ice/farmland). 0 = idle.
	bool fire_demo;
	int fire_demo_defer;
	// Debug side-contact lava scene; id/tick are telemetry only.
	bool mob_lava_demo;
	int mob_lava_demo_defer;
	uint32_t mob_lava_demo_id;
	int mob_lava_demo_ticks;
	// Debug ("debug.nether_demo", start_nether worlds): once at world load, build the Phase-0
	// nether-mechanics showcase ahead of the player (bed detonation crater + eternal netherrack
	// fire vs dying stone fire + soul-sand strip), aim-framed, no input. Set from main.c after
	// create; deferred via nether_demo_defer (mirrors fire/ice). 0 = idle.
	bool nether_demo;
	int nether_demo_defer;
	// Debug ("debug.portal_demo"): once at world load, build a lit portal pane
	// AROUND the aim-locked player -- the zero-input M5 travel showcase (live
	// ignition chokepoint -> contact -> 81-tick dwell -> the real dimension
	// switch on screen). Set from main.c after create; deferred via
	// portal_demo_defer (mirrors fire/nether). 0 = idle.
	bool portal_demo;
	// "debug.portal_demo" = "showcase": build the lit pane 5 blocks AHEAD of
	// the camera (face-on filming of the portal texture) instead of THROUGH
	// the player -- no travel fires.
	bool portal_demo_showcase;
	// "debug.portal_demo" = "split_dims" (split_dims_6m host switch,
	// splitdims-hostswitch-0927): the showcase pane is lit as above, and the
	// frame around the host is ignited too -- but only once the guest body
	// has walked out of it (portal_demo_host_pending), so only the host
	// travels. In the Nether the host stands in its arrival portal, where
	// Beta's setInPortal pins the cooldown at 10 forever; after
	// PORTAL_DEMO_SPLIT_NETHER_TICKS the demo zeroes that cooldown ONCE (the
	// portal_demo precedent) so contact re-arms the ramp and the host travels
	// back. Debug rig only; off = every field idle.
	bool portal_demo_split;
	bool portal_demo_host_pending;
	bool portal_demo_host_lit;
	bool portal_demo_rearmed;
	int portal_demo_hx, portal_demo_hy, portal_demo_hz;
	uint32_t portal_demo_nether_ticks;
	int portal_demo_defer;
	// M5: deferred nether-death world eviction. The death branch latches this
	// when the player dies IN the nether (Beta Minecraft.respawn's
	// !canRespawnHere() -> usePortal(); the dead player skips the Teleporter);
	// the tick top consumes it and round-trips the unload/load seam with the
	// already-stamped overworld spawn + dimension. Explicit default in
	// server_local_create (stack struct, never zeroed).
	bool death_world_evict;
	// Bounded retry delay for a fallible Nether-death close/checkpoint/reopen.
	// Live-world retries use normal 50ms ticks; positionless reopen retries use
	// the 2ms loading cadence and therefore receive a larger tick delay.
	int death_world_retry_delay;
	/* Runtime owner for a Nether-death transaction. PREPARING exists on disk
	 * before any close/drop mutation; prepared_durable gates every region write. */
	/* Lazily allocated in MEM2 when a Nether-death transaction starts. It must
	 * outlive the source-world teardown, then return to NULL after the durable
	 * journal is removed or ownership is handed to disk recovery. */
	struct death_drop_journal* death_drop_journal;
	bool death_drop_active;
	bool death_drop_prepared_durable;
	/* Invariant-failure escape hatch: discard the uncheckpointed live source and
	 * let startup recovery complete the already-durable PREPARED plan. */
	bool death_drop_recover_from_disk;
	size_t death_drop_transfer_index;
	/* Monotonic death-transaction id counter (bug-hunt D6): highest id ever
	 * ISSUED for this world, mirrored from/persisted to level.dat
	 * (LEVEL_CAVEWII_DEATH_TX_COUNTER) BEFORE the id is used, so region-file
	 * tags can never be reissued against a later dict-only scan. Loaded at
	 * world open (absent tag -> CAVEWII_DEATH_TX_LEGACY_FLOOR); recovery bumps
	 * it past a replayed journal's id to close the level.dat-rollback edge. */
	uint32_t death_tx_counter;
	// Debug ("debug.superflat"): generate a flat bedrock/dirt/grass world (no
	// caves/trees/ores) -- a clean stage for testing mobs. Threaded into
	// server_world_create. Read once on the main thread.
	bool superflat;
	// Debug ("debug.perf_stress"): once at world load, spawn the mob cap ringed
	// around the idle player + detonate one TNT-power explosion in the spawn chunk
	// -- a deterministic, no-input worst-case load so the P0 perf instruments
	// (srv tick ms, setblk us/peak, getblk/tick) can be read off F3 on Dolphin/Wii.
	// Read once on the main thread, like the other demos.
	bool perf_stress;
	/* Diagnostic v1 refill + lethal-debit guard. Default OFF; never gameplay. */
	bool perf_guard_requested, perf_guard_local_arm, perf_guard_armed;
	char perf_guard_root[256];
	uint32_t perf_guard_words[47];
	// M1 deep reach ("lod.far_mode"): when true (default), the far ring uses the
	// CHEAP terrain-only skin gen (server_world_extract_far_skin -- 1 chunk, no
	// populate cascade; render-only LOD, sim byte-identical on promotion). When false
	// ("populated") it uses the faithful full ensure_populated cascade per far send
	// (decoration in the distant skin, but ~10x more expensive). The toggle exists to
	// A/B the near<->far tree-line seam and as the documented LOD carve-out switch.
	bool far_lod_terrain_only;
	// Overlay an APPROXIMATE tree-trunk silhouette on the cheap far skin so distant
	// forests aren't bald (render-only LOD; trunks reposition to real trees on
	// promotion). Only meaningful when far_lod_terrain_only. Config "lod.far_mode":
	// "trees" (default, this on) | "terrain" (cheap bald, this off) | "populated".
	bool far_lod_trees;
	// Runtime far-ring distance in chunks (config "lod.far_view_distance"). The far
	// annulus is (MAX_VIEW_DISTANCE, far_view_distance]; == MAX_VIEW_DISTANCE -> off.
	// Defaults to MAX_FAR_VIEW_DISTANCE; clamped to [MAX_VIEW_DISTANCE, CAP] in main.c.
	int far_view_distance;
	// W1-c (far-reach program Wave 1, docs/far_reach_program_2026-09-03.md):
	// the cached nearest-first far-pick frontier. far_pick_template holds
	// every (dx,dz) offset in the annulus (MAX_VIEW_DISTANCE,
	// far_view_distance], sorted by squared distance ascending (ties broken
	// z-then-x, matching the old loop's raster scan order). Rebuilt only
	// when far_view_distance changes (far_pick_template_built_for tracks
	// the value it was last built for; -1 = never built). Offsets, not
	// absolute coordinates, so a player chunk crossing needs no rebuild --
	// see the far send loop in server_local.c for the full rationale.
	struct far_pick_offset far_pick_template[FAR_PICK_TEMPLATE_MAX];
	int far_pick_template_count;
	int far_pick_template_built_for;
	// T2 far-send share (2026-09-04, lane/far-send-share; config
	// "lod.far_send_near_owed", --cw-far-send-near-owed). The far send batch used to
	// be gated on a STRICT near_complete (every chunk of the near square already
	// sent); while walking, the near frontier never converges, so on real hardware
	// (far_walk_6m 2026-09-04) the whole reach-6 annulus stayed missing for the entire
	// walk. This is the maximum number of still-unsent near chunks under which the far
	// ring may take the near loop's LEFTOVER per-tick send budget. 0 reproduces
	// the old strict gate EXACTLY -- near_owed <= 0 is precisely near_complete --
	// and remains a fully supported opt-out. Default flipped to 8 on 2026-09-04
	// (lane/far-share-default-8) -- N=16 was measured to fill the far ring but
	// peak the cost (fps floor 10.4 -> 5.3); 8 is the bounded setting; see
	// docs/platform_perf_notes.md.
	int far_send_near_owed;
	// Sticky per-axis chunk-crossing heading (-1/0/+1) and the player chunk it was
	// last derived from, for the promotion-debt guard above. The server does NOT
	// mirror the client's walk velocity (player.vel_* is knockback only), so the
	// heading is derived from the player's own chunk crossings. far_send_heading_valid
	// is false until the first tick establishes a baseline chunk.
	w_coord_t far_send_last_cx;
	w_coord_t far_send_last_cz;
	int8_t far_send_heading_x;
	int8_t far_send_heading_z;
	bool far_send_heading_valid;
	// Per-game-tick FLOWING-FLUID budget for the scheduled-tick drain (config
	// "sim.scheduled_tick_budget"; default FCAVEX_SCHEDULED_TICK_BUDGET). 0 = unlimited
	// (the old behaviour: a fluid cascade fires every ready tick in one game-tick = the
	// 6-8s freeze). When set, only flowing water/lava beyond this count are deferred;
	// timing-sensitive non-fluid work passes through, subject to Beta's absolute
	// 1000-fire cap. Pure-fluid cohorts conserve their fixed point; mixed callbacks
	// can overtake deferred fluid and are the documented Wii timing deviation.
	uint32_t scheduled_tick_budget;
	/* pool_selftest_3m only: armed before the server thread starts. */
	bool pool_selftest_freeze;
	// AUTOSAVE (2026-06-12 persistence audit: persistence was QUIT-PATH-ONLY,
	// so a freeze/power-cut lost the whole session; Beta autosaves every 40
	// ticks). Every autosave_ticks server ticks (config "sim.autosave_seconds"
	// x 20; default 600 = 30s; 0 = off) the KB-scale state is checkpointed
	// (player + level.dat via the safe rotation + chests/signs.dat) and a
	// chunk SWEEP is armed that saves ONE modified resident chunk per tick
	// (erase=false, stays resident) until a pass finds none -- bounded SD I/O
	// per tick. The 30s + spread cadence (vs Beta's 2s full save) is a
	// documented Wii SD-write adaptation: the saved STATE is identical, only
	// the checkpoint interval differs.
	uint32_t autosave_ticks;
	uint32_t autosave_timer;
	bool autosave_sweep;
	// FIX #3(b): the sweep above re-scanned s->world.chunks for `modified`
	// FRESH every tick with no memory of what it had already saved THIS
	// interval. A chunk that re-dirties every tick on its own (an active
	// redstone clock, flowing water/lava) was therefore found "still dirty"
	// on every pass and kept `found` (and so autosave_sweep) true forever --
	// not a data-loss bug (each save still only touched one chunk per tick),
	// but it meant that ONE hot chunk was re-serialized to the SD card 20x/s
	// indefinitely instead of once per configured interval, AND its constant
	// re-appearance-as-nearest-dirty could starve a genuinely-once-dirtied
	// chunk elsewhere in dict iteration order from ever getting ITS turn
	// within the interval. Reset (dict_autosave_saved_reset) the moment a new
	// interval arms the sweep; the scan then skips any id already present
	// here even if `modified` is true again, so each chunk saves at most once
	// per autosave interval and the sweep provably disarms every interval.
	dict_autosave_saved_t autosave_saved_this_epoch;
	/* H3 (Fable A2T-SERVER-REVIEW): the same set per SECONDARY world slot (a
	 * chunk id names a column in every world); index = slot, [0] unused (slot
	 * 0 keeps the field above). Reset with it at each interval. */
	dict_autosave_saved_t autosave_saved_this_epoch_w[SERVER_LOCAL_WORLD_SLOTS];
	// Arrows fired by mob AI (e.g. skeleton shots) DURING the entity tick are
	// queued here, not inserted into `entities` directly: inserting mid-iteration
	// can resize the dict and invalidate the tick loop's live iterator (a hang).
	// Flushed into real entities right after the tick loop.
	struct pending_arrow {
		vec3 pos, vel;
		uint32_t owner_id;
		uint16_t item_id;
		int damage;
		bool from_player;
		/* S1-a2: WHICH actor fired it, stamped at FIRE time and copied onto
		 * entity_arrow.shooter_actor by server_local_flush_arrows. Zero (host)
		 * for every mob-fired arrow and for a host shot. */
		struct actor_ident shooter;
		bool from_skeleton; // shooter-was-a-skeleton, resolved at FIRE time (the
		                    // shooter is alive then by construction) -- Beta's
		                    // EntityArrow.shootingEntity reference equivalent for
		                    // the creeper record drop. Resolved at impact it would
		                    // miss whenever the skeleton died mid-flight.
		/* A2-E: the SPAWNING world, stamped at fire time from the shooter's
		 * resolved world (owner 0 = the host's world). The flush constructs
		 * into the world this handle resolves to and DROPS the job when it no
		 * longer resolves (retired generation). generation 0 = the legacy
		 * unbound stamp (a fixture whose slot 0 carries no generation): the
		 * routed world, exactly as before. */
		struct server_world_handle world_h;
		/* I02: when set, `vel` is unused at queue time: the flush draws
		 * Beta's setArrowHeading spread from the NEW arrow's own stream
		 * (discarding `spread_discard` gaussians first -- the skeleton's
		 * EntityArrow(world, living) constructor heading) and moves `pos`
		 * by the resulting velocity when `spread_advance` is set. */
		bool own_spread;
		bool spread_advance;
		uint8_t spread_discard;
		float spread_aim[3];
		float spread_speed, spread_deviation;
	} pending_arrows[16];
	int pending_arrow_count;
	// GHAST fireballs launched from the entity tick (batch 1b): queued for the
	// same no-mid-tick-insert reason as the arrows, flushed right after them.
	// dir is the UNNORMALIZED aim vector (entity_fireball normalizes).
	struct pending_fireball {
		vec3 pos;
		float dir[3];
		uint32_t owner_id;
		struct server_world_handle world_h; /* A2-E: see pending_arrow.world_h */
	} pending_fireballs[8];
	int pending_fireball_count;
	// MONSTERS spawned by entity-tick code (the slime death split) are queued
	// like the arrows above and flushed right after them -- same rationale
	// (no mid-tick dict insert; children spawn unticked this tick; the CRPC
	// order parent-DESTROY then child-SPAWNs stays deterministic). 16 slots
	// bounds a worst case of four 4-child splits in one tick.
	struct pending_monster {
		vec3 pos;
		int monster_id;
		struct server_world_handle world_h; /* A2-E: see pending_arrow.world_h */
	} pending_monsters[16];
	int pending_monster_count;
	/*
	 * Growable key worklist for the entity pass. A live dict iterator is unsafe
	 * because entity ticks can rehash the dict; a fixed 512-key snapshot froze
	 * the tail of dense TNT chains. New ids allocated while the pass is active
	 * append here, matching Beta World.updateEntities' live-list-size loop: chain
	 * TNT receives its first fuse/movement update later in the same world tick.
	 * The buffer is persistent MEM2 scratch, grown only when demand increases.
	 */
	uint32_t* entity_tick_keys;
	size_t entity_tick_count;
	size_t entity_tick_capacity;
	size_t entity_tick_reserved;
	/* Dictionary slots promised to detached EntityTNTPrimed reservations. Every
	 * ordinary insertion reserves around this count, so callbacks cannot consume
	 * the no-allocation commit capacity after a TNT block has become AIR. */
	size_t entity_dict_reserved;
	bool entity_tick_active;
	/* Fair cursors for the bounded post-physics minecart correction scheduler.
	 * Entity ids are monotonic within a world; selection wraps to the minimum id
	 * after the cursor, so hash-table iteration order cannot starve a cart. */
	uint32_t minecart_snapshot_priority_cursor;
	uint32_t minecart_snapshot_background_cursor;
	/* Monotonic allocator for mount-scoped ride epochs. Empty carts expose epoch
	 * zero, so the next value must live outside the entity or every remount would
	 * reuse epoch one and accept a delayed packet from the previous ride. */
	uint32_t next_minecart_ride_epoch;
	/* Growable snapshot used by World.getEntitiesWithinAABBExcludingEntity.
	 * It has no gameplay cap. Allocation failure leaves the current moving TE
	 * unadvanced so the exact same query retries on the next world tick. */
	struct server_entity_query_ref* entity_query_refs;
	size_t entity_query_count;
	size_t entity_query_capacity;
	uint64_t next_chunk_list_order;
	/* next-gaps-0924/W2 P4b (Astra item 13, "exact LOCAL broad-phase"): Beta
	 * Chunk.entities[section] -- one append-ordered id list per (chunk X,
	 * section, chunk Z), keyed CHUNK_TO_ID(cx, cy, cz). Appended by the two
	 * membership refreshes (the same moment list_order is assigned, so
	 * append order == list_order order); entries are validated lazily
	 * against the live dict/membership on every query and dropped when
	 * stale, so no erase site needs a hook. Lazily initialised on first use
	 * (hand-built fixtures memset this struct), cleared with the entity
	 * dict at both world resets. The exhaustive ordered snapshot above stays
	 * the equivalence reference (tests/test_entity_query_local_equiv.c).
	 * A2-E: ONE section index PER WORLD SLOT (Beta: Chunk.entities[] belongs
	 * to a World; the same section key in two worlds is two lists), so a
	 * same-coordinate query/collide push in slot 1 never sees slot 0's
	 * entities. The index of a world that is discarded and re-created holds
	 * only stale ids, dropped lazily (their entities no longer resolve). */
	dict_entity_sections_t entity_sections[SERVER_LOCAL_WORLD_SLOTS];
	bool entity_sections_ready[SERVER_LOCAL_WORLD_SLOTS];
	// Near resident evictions free the server chunk immediately, then must reliably
	// tell the client to unload the matching full chunk. If the client inbox token
	// pool is full, queue the unload here and retry next tick instead of blocking
	// the server thread inside clin_rpc_send.
	struct pending_chunk_unload {
		w_coord_t x, z;
	} pending_near_unloads[NEAR_UNLOAD_PENDING_MAX];
	int pending_near_unload_count;
	// Back off failed eviction saves so a persistent media error cannot
	// serialize the same near/far chunk every server tick.
	int chunk_save_retry_delay;
	/* Ticks the current SAVE BATCH has been open. The batch is published when
	 * this reaches SAVE_BATCH_MAX_TICKS (see its doc comment for the cost/
	 * exposure tradeoff); region_archive_sync/_destroy publish independently,
	 * so this counter only decides the CADENCE, never whether a write lands. */
	int save_batch_ticks;
	/* Failed saves in this respawn drain. Exhaustion retires the load as failed,
	 * retaining unsaved owners rather than filling a second resident ring. */
	unsigned respawn_drain_save_failures;
	// Near-send FIX #2: bounded backoff table for the nearest-unsent-chunk
	// scan in server_local_update. A chunk_id lands here (used[i]=true) ONLY
	// once its consecutive ensure_populated failure streak (struct
	// server_chunk's near_send_fail_streak, server_world.h) reaches
	// NEAR_SEND_FAIL_STREAK_THRESHOLD (server_local.c) -- a chunk failing for
	// the first few times is excluded only for the rest of the CURRENT tick
	// via its own near_send_fail_seq field, never touching this table at all.
	// retry_seq[i] names the near_send_seq value it becomes eligible for
	// re-selection again; the scan's predicate excludes any entry whose
	// deadline hasn't been reached yet. Explicitly CLEARED (used[i]=false) the
	// moment that chunk's ensure_populated call next succeeds (see
	// server_local_near_send_clear), so an ordinary walk that exposes many new
	// edge columns -- each failing once or twice before generation catches up,
	// completely normal -- never leaves stale entries occupying these 16 slots.
	// Prevents one persistently-deferring chunk (e.g. one needing heavy fresh
	// generation, or genuinely stuck) from freezing the whole near-send batch
	// behind it forever -- see server_local_near_send_is_backed_off /
	// _mark_failed / _clear. Round-robin eviction (next_slot) when full is a
	// bounded, honest degradation.
	//
	// FIX (regression): the deadline is measured against near_send_seq (see
	// its own doc comment below), NEVER world.current_tick and NEVER wall
	// time. current_tick is deliberately frozen for the entire duration of a
	// world-switch/initial load ("Suspended while loading (the world clock is
	// frozen)" -- server_local_update's `loading` gate above
	// server_world_tick()) -- which is exactly the scenario this table exists
	// to help (a dimension switch needs fresh generation across most/all of
	// the near ring). Using current_tick as the deadline meant a chunk
	// marked backed-off during loading NEVER became eligible again for the
	// rest of that same load: the "now" value compared against its deadline
	// never advanced, so once every near-ring chunk needing generation had
	// failed once and been backed off, the scan could never find a candidate
	// again and the loading screen hung forever (test_portal_travel.c's
	// finish_real_world_load timeout at its CRPC_PLAYER_POS wait). A first
	// fix attempt switched to wall-clock (ptime_t/time_get()) instead, which
	// again advances during loading -- but broke under test_portal_travel's
	// own fast-forward harness, which calls server_local_test_update_once()
	// back-to-back with no inter-tick sleep specifically so a bounded loop of
	// update() CALLS (not a bounded span of real time) is what must converge;
	// under that harness thousands of calls can complete in far less than the
	// backoff's ~1 real second, so the deadline still never arrived within
	// the test's call budget. near_send_seq (a call counter, not a clock)
	// converges correctly under both: it advances once per update() call
	// regardless of `loading`, wall-clock speed, or test-harness pacing.
	struct {
		int64_t chunk_id[NEAR_SEND_FAIL_TABLE_SIZE];
		uint32_t retry_seq[NEAR_SEND_FAIL_TABLE_SIZE];
		bool used[NEAR_SEND_FAIL_TABLE_SIZE];
		int next_slot;
	} near_send_fail;
	// Near-send FIX (regression) backing counter: increments exactly once per
	// server_local_update() call, UNCONDITIONALLY -- including while loading,
	// unlike world.current_tick (frozen while !finished_loading; see
	// near_send_fail's doc comment above for why that distinction matters).
	// Used only as the "now" input to server_local_near_send_is_backed_off /
	// _mark_failed. Deliberately never reset at the world-switch boundary:
	// it is a plain monotonic call count, not tied to any world session, so
	// there is nothing to invalidate (near_send_fail itself IS reset there,
	// which is what actually matters for the stale-chunk_id concern).
	uint32_t near_send_seq;
	uint32_t startup_retired, startup_save_failed;
	// Harden audit #3: monotonic fresh-entity-id counter. entity_gen_id() was an
	// O(N) full-dict max-key scan on EVERY spawn (~6 call sites). The server is the
	// only producer of fresh ids (the client receives server-assigned ids over RPC),
	// and ids are session-local (never persisted to the level archive), so a running
	// counter is the O(1), uniqueness-guaranteeing replacement: it hands out 1,2,3,...
	// in deterministic spawn order, never reuses a freed slot (safer than max+1, which
	// could alias an id whose CRPC_ENTITY_DESTROY is still in flight), and consumes no
	// RNG (the seeded stream is untouched). Seeded to 0 so the first id is 1 (id 0 is
	// reserved for the local player), exactly matching entity_gen_id's first output.
	// RESET to 0 alongside every dict_entity_reset(s->entities) (world load + unload),
	// so the "first id is 1" invariant holds PER WORLD, not just from server_local_create.
	// This is alias-safe because CRPC_WORLD_RESET clears the client's entity dict at the
	// same boundary (and the RPC queue is FIFO), so a reused id can't collide with a
	// surviving entity or an in-flight old-world DESTROY.
	uint32_t next_entity_id;

	// Debug capture aid: when non-zero, the server re-aims the player CAMERA at this
	// entity every tick (server_local_debug_cam_track) so a test subject (a chasing
	// mob) stays centered + well-sized in headless screenshots instead of sliding off
	// the bottom of a fixed-angle view. Set by a demo; 0 = off (no effect in play).
	uint32_t debug_cam_track_id;

	// Loading-screen pre-gen (the world-entry freeze fix): while the player has not
	// finished_loading, the server streams the whole spawn near-radius BEFORE handing
	// over control (the client stays on screen_load_world). load_wait_ticks is the
	// watchdog so a chunk that can never send (unreadable disk slot, persistent OOM)
	// can't hang the loading screen forever -- after LOAD_WATCHDOG_TICKS we enter anyway.
	int load_wait_ticks;

	// Cooperative destination-portal placement. The origin snapshot remains
	// valid until placement completes so any chunk/allocation failure can roll
	// the player back to the intact source world instead of ticking a partially
	// loaded destination.
	struct portal_teleporter_job* portal_job;
	enum server_world_switch_phase world_switch_phase;
	struct {
		bool valid;
		double x, y, z;
		float rx, ry;
		enum world_dim dimension;
		int fire, oxygen;
		double vel_x, vel_z; /* G8-BLAST-VELOCITY v3: server motion, double */
		float vel_y, old_vel_y;
		double motion_y; /* G8-BLAST-VELOCITY: server motionY */
		float fall_distance;
		bool on_ground, sneaking;
		float distance_walked;
		int next_step_distance;
		int time_until_portal;
		float time_in_portal;
		ptime_t transition_started_at; // hardware timing across cooperative steps
		// A transient archive/OOM failure while reopening the source must not
		// strand the loading screen. Retry at a bounded cadence, then retain the
		// durable origin behind a one-shot terminal load failure instead of
		// spinning forever.
		int rollback_retry_delay;
		unsigned rollback_retry_count;
	} portal_origin;

	/* A2-T / A2-W (server_transfer.h): one transaction per actor slot, the
	 * slot lifecycle states for slots >= 1 (slot 0 follows the legacy load
	 * path), which slots a transfer admitted, and the once-only service
	 * instruments (per world) + the session sleep-tick counter. */
	struct server_transfer_txn transfer[2];
	uint32_t transfer_rollback_id[2]; /* last rollback per actor, including closed requests */
	uint8_t transfer_rollback_reason[2];
	uint32_t transfer_next_id;
	uint32_t transfer_epoch;
	/* Admission keys reserved before one synchronous population cascade.
	 * A key becomes resident on insertion; only still-missing keys count as
	 * in-flight, so the same column is never counted twice. */
	struct server_transfer_column_reservation
		transfer_column_reservations[SERVER_TRANSFER_COLUMN_RESERVATIONS];
	uint8_t transfer_column_reservation_n;
	uint32_t transfer_column_cap_waits;
	bool transfer_column_budget_wait;
	uint32_t transfer_column_peak;
	enum server_transfer_budget_phase transfer_budget_phase;
	uint32_t transfer_prebudget_peak;
	enum server_world_slot_state world_slot_state[SERVER_LOCAL_WORLD_SLOTS];
	bool world_slot_admitted_by_transfer[SERVER_LOCAL_WORLD_SLOTS];
	struct server_world_service_stats world_service_stats[SERVER_LOCAL_WORLD_SLOTS];
	uint64_t session_sleep_ticks;
	/* M2 (Fable A2T-SERVER-REVIEW): client capacity tickets the server still
	 * owes a RELEASE for (a refused send retries every step, FIFO), and the
	 * ticket of each world slot's COMMITTED reservation (released when that
	 * slot is vacated or reserved again). */
	uint32_t transfer_cap_release_q[SERVER_TRANSFER_CAP_RELEASE_QUEUE];
	uint8_t transfer_cap_release_n;
	uint32_t transfer_slot_ticket[SERVER_LOCAL_WORLD_SLOTS];
	/* A2-W (A2W-CONTRACT.md): per-world streaming for slots >= 1. A world's
	 * stream to the client starts once the client holds its context (the
	 * host's stamped WORLD_RESET, the guest's gate-1 reset); the host's
	 * input-epoch marker after its COMMIT waits for the near ring of the
	 * host's OWN world (bounded by a_w_marker_wait). */
	bool a2w_stream_on[SERVER_LOCAL_WORLD_SLOTS];
	bool a2w_batch_owned[SERVER_LOCAL_WORLD_SLOTS]; /* only A2-W may close this window */
	bool a2w_reset_pending[SERVER_LOCAL_WORLD_SLOTS];
	uint32_t a2w_reset_generation[SERVER_LOCAL_WORLD_SLOTS];
	bool a2w_host_marker_pending;
	/* Host-only A2 presentation episode: a committed world-bound host is held
	 * from WORLD_RESET through its exact input ACK, including retry streams.
	 * The admitted guest continues on the ordinary server tick meanwhile. */
	bool a2w_host_presentation;
	bool a2w_guest_presentation;
	uint32_t a2w_host_marker_wait;
	uint32_t a2w_sent[SERVER_LOCAL_WORLD_SLOTS];	 /* columns streamed */
	uint32_t a2w_evicted[SERVER_LOCAL_WORLD_SLOTS]; /* columns evicted */
};


/* A saved Join owns streaming demand before it owns a gameplay body. */
static inline bool server_local_guest_join_streaming(const struct server_local* s) {
	return s && s->split_enabled && s->guest_owned && s->guest_confirmed
		&& s->guest_actor && s->guest_actor->has_pos
		&& !s->guest_actor->finished_loading && !s->guest_join_wait.failed;
}

/* TWO anchors serve the synthetic feasibility sweep, an admitted guest, or
 * a terrain-waiting saved Join. All require s->residency, allocated by
 * server_local_set_anchor_sep / server_local_set_split_enabled before boot,
 * or the server thread's SRPC_SPLIT_SET handler between ticks. */
static inline bool server_local_two_anchor(const struct server_local* s) {
	return s && (s->anchor_sep || s->guest_gameplay
		|| server_local_guest_join_streaming(s));
}

/* The EFFECTIVE server load radius (chunks). MAX_VIEW_DISTANCE stays the
 * compile-time CAP for every array/pool size; this is the value the
 * residency scheduler, near send, eviction keep radius and the client
 * readiness scans use. Single player: exactly MAX_VIEW_DISTANCE, so every
 * expression that reads it is textually what it was before (byte-identical
 * single-player behaviour by construction). Split: split_view_radius, never
 * above the cap. */
/* ===== A1b production tile view ==========================================
 * server_local_tile_store_in(s, w): the te_store view of session `s`'s
 * shared tile tables keyed by world `w` (a slot of `s`): every lookup /
 * replacement / preflight / gather / release through it sees only rows `w`
 * owns, and every insertion stamps `w`'s (slot, generation). A slot that is
 * not live yields a keyed view that owns and holds nothing -- never an
 * implicit fallback to another world.
 * server_world_tile_store(w): the same view, reaching the tables through w's
 * session (or its archive binding `local` for fixtures that set only that). */
static inline struct te_store server_local_tile_store_in(struct server_local* s,
														 struct server_world* w) {
	if(!s || !w || w->world_index >= SERVER_LOCAL_WORLD_SLOTS
	   || &s->worlds[w->world_index] != w || w->generation == 0) {
		struct te_store none = {0};
		none.owner_keyed = true;
		return none;
	}
	struct te_owner_table* o = &s->te_owners;
	return (struct te_store) {
		.chest_pos = (struct te_pos*)s->chest_pos,
		.chest_items = (struct item_data*)s->chest_items,
		.max_chest = s->te_rows[TE_KIND_CHEST],
		.chest_slots = MAX_CHEST_SLOTS,
		.sign_pos = (struct te_pos*)s->sign_pos,
		.sign_texts = (char*)s->sign_texts,
		.max_sign = s->te_rows[TE_KIND_SIGN],
		.sign_size = SIGN_SIZE,
		.furnace_pos = (struct te_pos*)s->furnace_pos,
		.furnaces = s->furnaces,
		.max_furnace = s->te_rows[TE_KIND_FURNACE],
		.dispenser_pos = (struct te_pos*)s->dispenser_pos,
		.dispensers = s->dispensers,
		.max_dispenser = s->te_rows[TE_KIND_DISPENSER],
		.note_pos = (struct te_pos*)s->note_pos,
		.notes = s->notes,
		.note_prev_powered = s->note_prev_powered,
		.max_note = s->te_rows[TE_KIND_NOTE],
		.record_player_pos = (struct te_pos*)s->record_player_pos,
		.records = s->records,
		.max_record_player = s->te_rows[TE_KIND_RECORD_PLAYER],
		.spawner_pos = (struct te_pos*)s->spawner_te_pos,
		.spawners = s->spawner_tes,
		.max_spawner = s->te_rows[TE_KIND_SPAWNER],
		.piston_moving = &s->piston_moving,
		.owner_keyed = true,
		.owner_slot = w->world_index,
		.owner_generation = w->generation,
		.owners = {
			[TE_KIND_CHEST] = {o->chest_slot, o->chest_gen},
			[TE_KIND_SIGN] = {o->sign_slot, o->sign_gen},
			[TE_KIND_FURNACE] = {o->furnace_slot, o->furnace_gen},
			[TE_KIND_DISPENSER] = {o->dispenser_slot, o->dispenser_gen},
			[TE_KIND_NOTE] = {o->note_slot, o->note_gen},
			[TE_KIND_RECORD_PLAYER] = {o->record_slot, o->record_gen},
			[TE_KIND_SPAWNER] = {o->spawner_slot, o->spawner_gen},
		},
		.reserved = {
			[TE_KIND_CHEST] = s->chest_reserved,
			[TE_KIND_SIGN] = s->sign_reserved,
		},
		.grow = s->te_grow.kinds ? &s->te_grow : NULL,
		.grow_epoch = s->te_grow.epoch,
	};
}

/* IMP-TE-CAPS: the slab layout of server_local's seven tile tables (one
 * te_slab_kind per enum te_kind; tile_entities_store.h "growable backing").
 * Every kind carries its position array (fill 0xFF: y = -1 = free), its
 * payload(s), and its two owner-key arrays; chests/signs add the A2-S
 * reservation mask. Initial rows = the historical fixed table sizes, so the
 * flat-archive image (MAX_CHESTS/MAX_SIGNS rows) always fits. */
#define SL_TE_FIELD(member, type, view_member, fillb)                      \
	{sizeof(type), offsetof(struct server_local, member),                  \
	 offsetof(struct te_store, view_member), (fillb)}
#define SL_TE_FIELD_NOVIEW(member, type, fillb)                            \
	{sizeof(type), offsetof(struct server_local, member), TE_SLAB_NO_VIEW,  \
	 (fillb)}
#define SL_TE_OWNER_FIELDS(kind, slotm, genm)                              \
	{sizeof(uint8_t), offsetof(struct server_local, te_owners.slotm),      \
	 offsetof(struct te_store, owners) + (kind) * sizeof(struct te_owner_view) \
		 + offsetof(struct te_owner_view, slot),                           \
	 0},                                                                   \
	{sizeof(uint32_t), offsetof(struct server_local, te_owners.genm),      \
	 offsetof(struct te_store, owners) + (kind) * sizeof(struct te_owner_view) \
		 + offsetof(struct te_owner_view, generation),                     \
	 0}
#define SL_TE_CAP(kind, view_max)                                          \
	offsetof(struct server_local, te_rows) + (kind) * sizeof(int),         \
		offsetof(struct te_store, view_max)
__attribute__((unused))
static const struct te_slab_kind server_local_te_layout[TE_KIND_FIXED_COUNT] = {
	[TE_KIND_CHEST] = {SL_TE_CAP(TE_KIND_CHEST, max_chest), MAX_CHESTS, {
		SL_TE_FIELD(chest_pos, struct complex_block_pos, chest_pos, 0xFF),
		SL_TE_FIELD(chest_items, struct item_data[MAX_CHEST_SLOTS],
					chest_items, 0),
		SL_TE_OWNER_FIELDS(TE_KIND_CHEST, chest_slot, chest_gen),
		{sizeof(uint8_t), offsetof(struct server_local, chest_reserved),
		 offsetof(struct te_store, reserved) + TE_KIND_CHEST * sizeof(uint8_t*),
		 0},
	}},
	[TE_KIND_SIGN] = {SL_TE_CAP(TE_KIND_SIGN, max_sign), MAX_SIGNS, {
		SL_TE_FIELD(sign_pos, struct complex_block_pos, sign_pos, 0xFF),
		SL_TE_FIELD(sign_texts, char[SIGN_SIZE], sign_texts, ' '),
		SL_TE_OWNER_FIELDS(TE_KIND_SIGN, sign_slot, sign_gen),
		{sizeof(uint8_t), offsetof(struct server_local, sign_reserved),
		 offsetof(struct te_store, reserved) + TE_KIND_SIGN * sizeof(uint8_t*),
		 0},
	}},
	[TE_KIND_FURNACE] = {SL_TE_CAP(TE_KIND_FURNACE, max_furnace), MAX_FURNACES, {
		SL_TE_FIELD(furnace_pos, struct complex_block_pos, furnace_pos, 0xFF),
		SL_TE_FIELD(furnaces, struct furnace_te, furnaces, 0),
		SL_TE_OWNER_FIELDS(TE_KIND_FURNACE, furnace_slot, furnace_gen),
	}},
	[TE_KIND_DISPENSER] = {SL_TE_CAP(TE_KIND_DISPENSER, max_dispenser),
						   MAX_DISPENSERS, {
		SL_TE_FIELD(dispenser_pos, struct complex_block_pos, dispenser_pos, 0xFF),
		SL_TE_FIELD(dispensers, struct dispenser_te, dispensers, 0),
		SL_TE_OWNER_FIELDS(TE_KIND_DISPENSER, dispenser_slot, dispenser_gen),
	}},
	[TE_KIND_NOTE] = {SL_TE_CAP(TE_KIND_NOTE, max_note), MAX_NOTEBLOCKS, {
		SL_TE_FIELD(note_pos, struct complex_block_pos, note_pos, 0xFF),
		SL_TE_FIELD(notes, int8_t, notes, 0),
		SL_TE_FIELD(note_prev_powered, bool, note_prev_powered, 0),
		SL_TE_OWNER_FIELDS(TE_KIND_NOTE, note_slot, note_gen),
	}},
	[TE_KIND_RECORD_PLAYER] = {SL_TE_CAP(TE_KIND_RECORD_PLAYER,
										 max_record_player),
							   MAX_JUKEBOXES, {
		SL_TE_FIELD(record_player_pos, struct complex_block_pos,
					record_player_pos, 0xFF),
		SL_TE_FIELD(records, int32_t, records, 0),
		SL_TE_OWNER_FIELDS(TE_KIND_RECORD_PLAYER, record_slot, record_gen),
	}},
	[TE_KIND_SPAWNER] = {SL_TE_CAP(TE_KIND_SPAWNER, max_spawner),
						 MAX_SPAWNER_TES, {
		SL_TE_FIELD(spawner_te_pos, struct complex_block_pos, spawner_pos, 0xFF),
		SL_TE_FIELD(spawner_tes, struct spawner_te, spawners, 0),
		SL_TE_OWNER_FIELDS(TE_KIND_SPAWNER, spawner_slot, spawner_gen),
	}},
};
#undef SL_TE_FIELD
#undef SL_TE_FIELD_NOVIEW
#undef SL_TE_OWNER_FIELDS
#undef SL_TE_CAP

/* IMP-TE-CAPS: allocate session `s`'s growable tile slabs (initial MAX_*
 * rows each, every row free) and arm their growth. server_local_init calls
 * it; a test that hand-builds a server_local calls it instead of memset-ing
 * the old fixed arrays. False (logged) only on allocation failure. */
bool server_local_te_tables_init(struct server_local* s);
/* Same, on the plain C heap (malloc/free): for dependency-light tests that
 * link neither server_local.c nor mem2.c. */
static inline bool server_local_te_tables_init_heap(struct server_local* s) {
	return te_grow_ctx_init(&s->te_grow, s, server_local_te_layout, malloc,
							free);
}

static inline struct te_store server_world_tile_store(struct server_world* w) {
	struct server_local* s = w ? (w->session ? w->session : w->local) : NULL;
	return server_local_tile_store_in(s, w);
}

/* ===== A1b tile-window binding ===========================================
 * A tile window is bound to its opening world (server_player.window_world,
 * stamped by the open path before on_create). The EFFECTIVE world of a
 * window: its bound instance while that generation is live; NULL once the
 * bound instance is gone (stale -- only this window loses its backing) and
 * NULL for an UNBOUND window (no owner or no binding) -- A1c closure of the
 * last inventoried slot-0 fallback (Astra A1 "short closure", after lane C's
 * C1 bound the guest-restored windows, server_local_guest.c
 * guest_restore_window). A window that no open path bound FAILS CLOSED:
 * it has no backing, its publications go nowhere. Hand-built test windows
 * bind their owner (`owner->window_world = server_world_handle_of(w)`) to a
 * live slot-0 instance. */
static inline struct server_world* server_local_window_world(
	const struct inventory* inv) {
	if(!inv || !inv->user)
		return NULL;
	struct server_local* s = inv->user;
	if(!inv->owner || inv->owner->window_world.generation == 0)
		return NULL; /* unbound window: no world, fail closed */
	struct server_world_handle h = inv->owner->window_world;
	if(h.world_index >= SERVER_LOCAL_WORLD_SLOTS)
		return NULL;
	struct server_world* w = &s->worlds[h.world_index];
	return w->chunks_ready && w->generation == h.generation ? w : NULL;
}

static inline bool server_local_window_bound_to(const struct inventory* inv,
												const struct server_world* w) {
	return w && server_local_window_world(inv) == w;
}

static inline bool server_local_windows_share_world(const struct inventory* a,
													const struct inventory* b) {
	struct server_world* wa = server_local_window_world(a);
	return wa && wa == server_local_window_world(b);
}

/* Conservative form for REFUSAL gates (sort admission, replacement-open
 * retirement): only two windows that both resolve to DIFFERENT worlds are
 * known to be apart; an unresolved one (stale, or a hand-built view with no
 * session) may still share the chest and keeps refusing. */
static inline bool server_local_windows_may_share_world(
	const struct inventory* a, const struct inventory* b) {
	struct server_world* wa = server_local_window_world(a);
	struct server_world* wb = server_local_window_world(b);
	return !wa || !wb || wa == wb;
}

/* The window's backing view (keyed by its effective world; empty if stale). */
static inline struct te_store server_local_window_tile_store(
	const struct inventory* inv) {
	struct server_world* w = server_local_window_world(inv);
	return server_local_tile_store_in(w ? (struct server_local*)inv->user : NULL,
									  w);
}

static inline int server_local_chest_row_w(struct server_world* w, w_coord_t x,
										   w_coord_t y, w_coord_t z) {
	struct te_store st = server_world_tile_store(w);
	return te_store_find(&st, TE_KIND_CHEST, x, y, z);
}

static inline int server_local_sign_row_w(struct server_world* w, w_coord_t x,
										  w_coord_t y, w_coord_t z) {
	struct te_store st = server_world_tile_store(w);
	return te_store_find(&st, TE_KIND_SIGN, x, y, z);
}

/* The piston-store owner key for world `w` ({0,0} for an invalid world, which
 * matches only unstamped slot-0 entries -- callers pass validated worlds). */
static inline struct piston_moving_owner server_world_piston_owner(
	const struct server_world* w) {
	return w ? (struct piston_moving_owner) {w->world_index, w->generation}
			 : (struct piston_moving_owner) {0, 0};
}

static inline int server_local_view_radius(const struct server_local* s) {
	int r = MAX_VIEW_DISTANCE;
	/* The two-world backing budget can hold two radius-four rings plus 14
	 * transfer columns. Keep the configured dial for 1..4; 0/full and larger
	 * values resolve to four only while that opt-in mode is active. */
	if(s && clin_world_payloads_enabled()
	   && (s->split_enabled || s->guest_gameplay
		   || s->player.world_slot != 0
		   || s->transfer[0].open || s->transfer[1].open)) {
		r = s->split_view_radius;
		if(r <= 0 || r > 4)
			r = 4;
	} else if(s && server_local_two_anchor(s) && s->split_view_radius > 0)
		r = s->split_view_radius;
	return r > MAX_VIEW_DISTANCE ? MAX_VIEW_DISTANCE : r;
}

#endif
