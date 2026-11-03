/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CaveWii_v2.
*/

/* The one thing entity_local_player.c exports for OBSERVERS: the Beta
 * blockExists gate that decides whether the local player's body ticks at all.
 *
 * WHY THIS HEADER EXISTS (push-0921/B4c). local_player_column_present() was a
 * file static. It is the first thing entity_tick() evaluates -- when it says
 * false the body does NOTHING: no physics, no damage, and no motion send,
 * because Beta's own
 *     if(!worldObj.blockExists(floor(posX), 64, floor(posZ))) return;
 * [740c583 EntityClientPlayerMP.onUpdate -> World.blockExists ->
 * chunkExists(x >> 4, z >> 4)] skips super.onUpdate() and func_4056_N()
 * alike. A scripted driver (source/game/mp_play_script.c) has to know that,
 * because a body that is frozen behind this gate looks exactly like a body
 * standing still, and counting a "ready" world while it is frozen would let a
 * run play into a corpse-stiff player and report timings that never happened.
 *
 * OBSERVERS MUST CALL THIS, NEVER RE-DERIVE IT. A second copy of the
 * chunkExists gate is precisely the drift the F8b ruling closed (see the
 * function's own comment in entity_local_player.c for the full argument:
 * a Beta chunk is a whole COLUMN, this port's chunk is one SECTION, so the
 * faithful predicate is "at least one section of the column is present" and
 * NOT world_find_chunk(x, 64, z)). A private copy that disagreed would make
 * an observer report a body ticking that is frozen, or the reverse.
 *
 * Exporting it changes no behaviour whatsoever: the definition, the call in
 * entity_tick and the predicate itself are untouched -- only the `static`
 * keyword is gone.
 */

#ifndef ENTITY_LOCAL_PLAYER_H
#define ENTITY_LOCAL_PLAYER_H

#include <stdbool.h>

#include "../block/blocks_data.h" /* w_coord_t */
#include "../platform/time.h"

struct world;
struct input_player_frame;

/* Local creative only (ruling 912c987). Called once per rendered frame,
 * before tick catch-up, so short jump presses are not lost or repeated.
 * Slot 0/1 retain independent, non-persisted flight state. Uses the existing
 * JUMP/SNEAK bindings; SNEAK's configured toggle/hold level means descend. */
void entity_local_player_flight_input(unsigned slot,
	const struct input_player_frame* frame, ptime_t now);
/* Synchronizes lifecycle first: clears stale flight and, on creative exit,
 * removes a formerly flying local body's residual upward velocity. */
bool entity_local_player_flying(unsigned slot);

/* THE Beta blockExists gate. True when at least one section of the column
 * (x >> 4, z >> 4) is present in `w` -- i.e. when Beta's chunkExists would be
 * true and the local player's body is allowed to tick. `w` must not be NULL
 * (the implementation asserts it). */
bool local_player_column_present(struct world* w, w_coord_t x, w_coord_t z);

/* D1 (ASTRA-DEFECTS-RULING items 2-6; F3-1 controller adaptation): the HOST
 * ride action (the F3-1 JUMP cart dismount) is a one-frame input edge, but its
 * only consumer runs in the 20 Hz client tick, and a 60 Hz frame often runs
 * no tick (2 of 3 on the Wii) -- such a press was silently lost. main.c calls
 * entity_local_player_ride_action_frame() ONCE PER FRAME, immediately before
 * the tick catch-up loop: it latches an eligible edge into
 * gstate.players[0].ride, and the next host tick consumes it exactly once.
 * Held state (frame.down) and every frame-based UI edge are untouched.
 *
 * eligible(): the host is in live gameplay with input captured -- not paused,
 * not in a menu/pause/recovery (capture_input), not dead or respawn-loading,
 * not in a world switch/terrain hold, not transfer-held. An ineligible frame
 * CLEARS any pending latch (focus/menu/controller loss, death, transfer). */
bool entity_local_player_ride_action_eligible(void);
void entity_local_player_ride_action_frame(void);
/* The same capture at the END of the frame (main.c, right before
 * input_poll), for an edge a producer raised AFTER the tick section -- the
 * vehicle-smoke host driver and the autopilot script apply their button
 * levels mid-frame, after the tick catch-up, so such an edge was invisible to
 * every tick. Skipped when this frame's begin capture already saw an edge, so
 * one edge is never latched twice (online that would toggle the rider back
 * on). A real controller edge is published by input_poll and is therefore
 * always taken by the begin capture. */
void entity_local_player_ride_action_frame_end(void);

#endif
