/*
	Copyright (c) 2026

	This file is part of CavEX.
*/

#ifndef PROFILE_H
#define PROFILE_H

#ifdef FCAVEX_PROFILE_WII_LOW
#define FCAVEX_RANDOM_TICKS_PER_CHUNK 20
#define FCAVEX_MAX_EXPLOSIONS_PER_TICK 2
// Per-game-tick FLOWING-FLUID budget for the scheduled-tick drain. Beta caps all work
// at 1000/tick (World.tickUpdates); the single ~729MHz Wii core cannot fire 1000
// set_block+lighting-flood ticks in one game-tick without the multi-second freeze users
// hit when a fluid cascade (e.g. crossing into a big water body) makes the queue reach
// thousands. Cap lower so a pure-fluid cohort fires without loss, spread across more
// game-ticks; test_fluid_spread + test_scheduled_tick_budget pin its fixed point and
// conservation. This is the playability carve-out: Beta's value is unplayable on the
// Wii core, so it is tuned down and documented
// (config-tunable: "sim.scheduled_tick_budget", 0 = unlimited = old freezing behaviour).
//
// CLASS SPLIT: only moving water/lava IDs 8/10 spend the lower Wii cap. Ready
// fire and redstone entries behind that fluid backlog pass through in the same
// game tick, while the whole block drain still stops at Beta's 1000-fire ceiling.
// Entity explosion pacing has its own per-update cap: primed TNT and fully-fused
// creepers retain their source entity until admitted, while rare ownerless blasts
// remain synchronous. A full block ring therefore cannot drop a blast. This
// deliberately lets later cross-class callbacks overtake capped older fluids; mixed
// fluid+fire/redstone outcomes can therefore differ from Beta insertion order. That is
// an explicit, test-pinned Wii timing/playability deviation, not byte equivalence (see
// docs/wii_controller_playability.md #17).
//
// NOTE (2026-06-16, redstone review R2): an earlier version of this comment claimed
// the FIFO "never drops" -- that was FALSE (server_world_schedule_tick_at_raw drops
// at the 4096-entry cap). The real risk was that the queue had NO Beta-style
// dedup, so duplicate notifies could inflate occupancy and a tight clock sharing
// the ring with a fluid cascade could overflow -> drop a non-self-healing tick.
// FIXED: server_world_schedule_tick_at_raw now applies Beta's
// (x,y,z,block_type) containment gate (scheduledTickSet.contains), so a position's pending
// tick count is bounded to one and the duplicate-inflation overflow cannot happen
// (test_scheduled_tick_dedup_golden; full suite incl. test_fluid_spread green). A hard cascade can
// still legitimately reach the cap with thousands of DISTINCT cells; the class-split
// drain prevents that fluid backlog from stretching timing-sensitive ticks.
#define FCAVEX_SCHEDULED_TICK_BUDGET 128
#endif

#ifndef FCAVEX_RANDOM_TICKS_PER_CHUNK
#define FCAVEX_RANDOM_TICKS_PER_CHUNK 80
#endif

#ifndef FCAVEX_SCHEDULED_TICK_BUDGET
#define FCAVEX_SCHEDULED_TICK_BUDGET 1000 // Beta's own World.tickUpdates cap (fast cores)
#endif

#ifndef FCAVEX_MAX_EXPLOSIONS_PER_TICK
#define FCAVEX_MAX_EXPLOSIONS_PER_TICK 4
#endif

#endif
