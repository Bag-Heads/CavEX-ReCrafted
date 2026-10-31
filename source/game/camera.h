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

#ifndef CAMERA_H
#define CAMERA_H

#include <stdbool.h>

#include "../cglm/cglm.h"
#include "../entity/entity.h"
#include "../entity/player_ysize.h"
#include "camera_bob.h"
#include "hud_hurt.h"

struct camera {
	float x, y, z;
	float rx, ry;
	mat4 view, projection;
	vec4 frustum_planes[6];
	struct camera_controller {
		float vx, vy, vz;
	} controller;
	/* Beta view bobbing (camera_bob.h). bob_partial/bob_active are written by
	 * camera_attach each rendered frame and consumed (then cleared) by
	 * camera_update, so the detached free camera and menu screens never bob. */
	struct camera_bob_state bob;
	float bob_partial;
	bool bob_active;
	/* Beta hurt feedback (hud_hurt.h): heart blink/jitter state for the HUD
	 * and the hurt/death camera roll, ticked alongside the bob state from the
	 * same 20 Hz call site (Batch J). */
	struct hud_hurt_state hurt;
	/* Sleep/wake look belongs to this camera, not a function-static player. */
	bool was_sleeping;
	float wake_rx, wake_ry;
	/* Beta sneak camera dip (Entity.ySize -- player_ysize.h). Per LOCAL
	 * PLAYER, and this camera is exactly that: one per client_session, so the
	 * host and the split guest each own their own dip, like bob/hurt/wake
	 * above. Ticked by entity_local_player_walk_owned, applied (and its
	 * fold-out recorded) by camera_attach_owned, folded back out of the wire
	 * by clin_update. Presentation only -- the body never moves. */
	struct player_ysize_state ysize;
};


struct camera_ray_result {
	bool hit;
	w_coord_t x, y, z;
	enum side side;
	bool   entity_hit;
	uint32_t entity_id;
	/* Attack assist (docs/deviations.md "Soft-snap melee assist"): set ONLY
	 * when the direct pick missed everything (hit == entity_hit == false) and
	 * the opt-in snap found an eligible, unoccluded target. Consumed by the
	 * left-click ATTACK path and the crosshair tint only -- never by use,
	 * placement, mining or the bow, which keep reading the direct pick. */
	bool   assist_hit;
	uint32_t assist_entity_id;
};

void camera_ray_pick(struct world* w, float gx0, float gy0, float gz0,
					 float gx1, float gy1, float gz1,
					 struct camera_ray_result* res);
/* O17: the block-cell traversal behind camera_ray_pick, with the per-cell
 * hit test injected (camera_ray_pick passes world_block_intersection). Pure:
 * no world access of its own, so it is testable without a client world. */
struct ray;
typedef bool (*camera_ray_cell_fn)(void* user, w_coord_t x, w_coord_t y,
								   w_coord_t z, struct ray* r, enum side* s);
void camera_ray_walk(float gx0, float gy0, float gz0, float gx1, float gy1,
					 float gz1, camera_ray_cell_fn cell, void* user,
					 struct camera_ray_result* res);
void camera_physics(struct camera* c, float dt);
void camera_update(struct camera* c, bool in_water);
bool camera_update_viewport(struct camera* c, bool in_water, int width, int height);
struct minecart_ride_client_state;
struct camera_player_view {
	struct world* world;
	struct minecart_ride_client_state* ride;
	bool sleeping;
	float yaw, pitch; /* this player's resolved look, in radians */
};
void camera_attach_owned(struct camera* c, struct entity* e, float tick_delta,
	float dt, const struct camera_player_view* owner);
void camera_attach(struct camera* c, struct entity* e, float tick_delta,
				   float dt);
/* Once per 20 Hz client tick, after entities_client_tick: feed the local
 * player's tick results into the view-bob state (camera_bob.h). NULL player
 * decays the sway to rest exactly like Beta's dead-player gates. */
void camera_bob_client_tick(struct camera* c, const struct entity* player);
void camera_get_ray(const struct camera *c, vec3 origin, vec3 dir);

#endif
