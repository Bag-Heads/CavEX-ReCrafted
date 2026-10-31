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


#ifndef ENTITY_MINECART_H
#define ENTITY_MINECART_H

#include <stdbool.h>
#include "../cglm/cglm.h"
#include "entity.h"

struct minecart_rail_pose {
	float yaw_deg;
	float pitch_deg;
	float y_offset;
};

/* Resolve the visual pose for a cart over a rail block.  Kept separate from
 * drawing so all three Beta rail types share exactly the same metadata rules. */
void entity_minecart_rail_pose(uint8_t block_type, uint8_t metadata,
							  struct minecart_rail_pose* pose);

void entity_minecart(uint32_t id, struct entity* e, bool server, void* world);
/* G2: Beta EntityMinecart.onUpdate's end-of-tick collision sweep [CFR
 * EntityMinecart.java:441-447] plus the living half that Beta runs from
 * EntityLiving.onLivingUpdate (cart.applyEntityCollision(mob), :554-612).
 * Run by the server cart tick; exported for tests. */
struct server_local;
void entity_minecart_collide_neighbours(struct server_local* s,
										struct entity* cart);


/* Rider intent (+1 forward / -1 back) -> signed step along the cart's heading
 * axis. Shared by the client input path and its tests. */
int8_t entity_minecart_resolve_throttle(int8_t intent, float speed, int8_t hx,
										int8_t hz, float yaw);

/* Stage the bounded locally-authored suffix from an authoritative snapshot.
 * clin_update runs before the 20 Hz client entity pass, so the upcoming
 * ordinary cart tick consumes these steps exactly once through shared physics.
 * Running physics directly from the RPC handler would double-tick the cart in
 * any frame that also reaches entities_client_tick. */
void entity_minecart_stage_replay_control(struct entity* e, int32_t steps);

/* Re-enter the server's bounded snapshot rotation after any authoritative
 * state change outside ordinary rail physics (piston today; future collision
 * or explosion impulses). urgent is reserved for ownership/epoch barriers. */
void entity_minecart_mark_snapshot_dirty(struct entity* e, bool urgent);

/* Rolling-sound speed -> (volume, pitch) curve (DELIBERATE NON-BETA ADD-ON;
 * see audio_sound_table.h / docs/deviations.md). Pure and shared with the
 * unit test. */
void entity_minecart_roll_volume(float speed, float* vol_out,
								  float* pitch_out);

/* Whether the "inside" seat-loop add-on should fire for a cart in the given
 * occupancy state, given the LOCAL player's own entity id. Pure key-selection
 * predicate, shared with the unit test -- see minecart_inside_sound. */
bool entity_minecart_wants_inside_sound(bool occupied, uint32_t occupant_id,
										 uint32_t local_player_id);

#endif
