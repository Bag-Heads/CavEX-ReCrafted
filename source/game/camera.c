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

#include <assert.h>

#include "../platform/gfx.h"
#include "../block/bed_meta.h"
#include "../platform/input.h"
#include "camera.h"
#include "game_state.h"

#ifdef CAVEWII_AGENT_BRIDGE
#include "agent_input.h"
#endif

#include "../cglm/cglm.h"

// all vectors normalized
/* O17 (vehicle-smoke-1004): the distance along the ray to the next cell
 * plane on one axis. Only a ZERO or receding component means "never" (a
 * correct DDA's infinite tMax). A tiny positive one (a float-trig look that
 * is almost, but not exactly, axis-parallel: cos(-pi/2) = -4.4e-8) is a REAL
 * crossing at a large finite distance -- and a zero one if the eye sits on
 * that plane. The old `d < FLT_EPSILON -> FLT_MAX` declared such an axis
 * parallel, so it never stepped although the segment's end cell is across
 * it: the walk missed its end cell and ran off forever (main-thread freeze,
 * eye on an integer coordinate looking along an axis; test_camera_ray_pick).
 * For every |component| >= FLT_EPSILON this returns exactly what it did. */
static float plane_distance(vec3 n, vec3 p0, vec3 l0, vec3 l) {
	float d = glm_vec3_dot(n, l);

	if(d <= 0.0F)
		return FLT_MAX;

	vec3 tmp;
	glm_vec3_sub(p0, l0, tmp);
	return glm_vec3_dot(tmp, n) / d;
}

static bool camera_ray_world_cell(void* user, w_coord_t x, w_coord_t y,
								  w_coord_t z, struct ray* r, enum side* s) {
	return world_block_intersection((struct world*)user, r, x, y, z, s);
}

void camera_ray_pick(struct world* w, float gx0, float gy0, float gz0,
					 float gx1, float gy1, float gz1,
					 struct camera_ray_result* res) {
	assert(w && res);
	camera_ray_walk(gx0, gy0, gz0, gx1, gy1, gz1, camera_ray_world_cell, w, res);
}

void camera_ray_walk(float gx0, float gy0, float gz0, float gx1, float gy1,
					 float gz1, camera_ray_cell_fn cell, void* user,
					 struct camera_ray_result* res) {
	assert(cell && res);
	int sx = gx1 > gx0 ? 1 : -1;
	int sy = gy1 > gy0 ? 1 : -1;
	int sz = gz1 > gz0 ? 1 : -1;

	int gx = floor(gx0);
	int gy = floor(gy0);
	int gz = floor(gz0);

	int x1 = floor(gx1);
	int y1 = floor(gy1);
	int z1 = floor(gz1);

	vec3 dir = {gx1 - gx0, gy1 - gy0, gz1 - gz0};
	glm_vec3_normalize(dir);

	/* O17: a DDA from cell A to cell B visits exactly |B-A|_1 + 1 cells. If
	 * float rounding ever steers the walk off that path, end the walk there
	 * (no pick) instead of walking forever. Beta's rayTraceBlocks is likewise
	 * hard-capped (200 steps, CFR World.java func_28099_a). */
	const long max_cells = labs((long)x1 - gx) + labs((long)y1 - gy)
		+ labs((long)z1 - gz) + 1;
	long visited = 0;

	while(visited++ < max_cells) {
		// Initialized to silence -Wmaybe-uninitialized: every true return of
		// world_block_intersection writes *s through aabb_intersection_ray,
		// but that helper's if/else-if chain has no final else, so GCC cannot
		// prove it. SIDE_BOTTOM is a valid side if the unproven path ever ran.
		enum side s = SIDE_BOTTOM;
		// TODO:
		if(cell(user, gx, gy, gz,
				&(struct ray) {.x = gx0,
							   .y = gy0,
							   .z = gz0,
							   .dx = dir[0],
							   .dy = dir[1],
							   .dz = dir[2]},
				&s)) {
			res->x = gx;
			res->y = gy;
			res->z = gz;
			res->hit = true;
			res->side = s;
			return;
		}

		if(gx == x1 && gy == y1 && gz == z1) {
			res->hit = false;
			return;
		}

		float t1 = plane_distance((vec3) {sx, 0, 0},
								  (vec3) {gx + (sx + 1) / 2, 0, 0},
								  (vec3) {gx0, gy0, gz0}, dir);
		float t2 = plane_distance((vec3) {0, sy, 0},
								  (vec3) {0, gy + (sy + 1) / 2, 0},
								  (vec3) {gx0, gy0, gz0}, dir);
		float t3 = plane_distance((vec3) {0, 0, sz},
								  (vec3) {0, 0, gz + (sz + 1) / 2},
								  (vec3) {gx0, gy0, gz0}, dir);

		if(t1 <= t2 && t1 <= t3) {
			gx += sx;
		} else if(t2 <= t1 && t2 <= t3) {
			gy += sy;
		} else if(t3 <= t1 && t3 <= t2) {
			gz += sz;
		}
	}
	res->hit = false; /* O17: walked the full cell budget without the end cell */
}

void camera_physics(struct camera* c, float dt) {
	assert(c);

	float jdx, jdy;
	if(input_joystick(dt, &jdx, &jdy)) {
		c->rx -= jdx * 2.0F;
		c->ry -= jdy * 2.0F;
	}

	float acc_x = 0, acc_y = 0, acc_z = 0;
	float speed_c = 40.0F;
	float air_friction = 0.05F;

	if(input_held(IB_LEFT)) {
		acc_x += cos(c->rx) * speed_c;
		acc_z -= sin(c->rx) * speed_c;
	}

	if(input_held(IB_RIGHT)) {
		acc_x -= cos(c->rx) * speed_c;
		acc_z += sin(c->rx) * speed_c;
	}

	if(input_held(IB_FORWARD)) {
		acc_x += sin(c->rx) * sin(c->ry) * speed_c;
		acc_y += cos(c->ry) * speed_c;
		acc_z += cos(c->rx) * sin(c->ry) * speed_c;
	}

	if(input_held(IB_BACKWARD)) {
		acc_x -= sin(c->rx) * sin(c->ry) * speed_c;
		acc_y -= cos(c->ry) * speed_c;
		acc_z -= cos(c->rx) * sin(c->ry) * speed_c;
	}

	if(input_held(IB_JUMP))
		acc_y += speed_c;

	if(input_held(IB_SNEAK))
		acc_y -= speed_c;

	c->controller.vx += acc_x * dt;
	c->controller.vy += acc_y * dt;
	c->controller.vz += acc_z * dt;

	c->controller.vx *= powf(air_friction, dt);
	c->controller.vy *= powf(air_friction, dt);
	c->controller.vz *= powf(air_friction, dt);

	struct AABB bbox;

	aabb_setsize_centered(&bbox, 0.6F, 0.6F, 0.6F);
	aabb_translate(&bbox, c->x + c->controller.vx * dt, c->y, c->z);
	if(!world_aabb_intersection(&gstate.world, &bbox)) {
		c->x += c->controller.vx * dt;
	} else {
		c->controller.vx = 0;
	}

	aabb_setsize_centered(&bbox, 0.6F, 0.6F, 0.6F);
	aabb_translate(&bbox, c->x, c->y + c->controller.vy * dt, c->z);
	if(!world_aabb_intersection(&gstate.world, &bbox)) {
		c->y += c->controller.vy * dt;
	} else {
		c->controller.vy = 0;
	}

	aabb_setsize_centered(&bbox, 0.6F, 0.6F, 0.6F);
	aabb_translate(&bbox, c->x, c->y, c->z + c->controller.vz * dt);
	if(!world_aabb_intersection(&gstate.world, &bbox)) {
		c->z += c->controller.vz * dt;
	} else {
		c->controller.vz = 0;
	}

	c->ry = glm_clamp(c->ry, glm_rad(0.5F), GLM_PI - glm_rad(0.5F));
}

void camera_update(struct camera* c, bool in_water) {
	assert(c);
	(void)camera_update_viewport(c, in_water, gfx_width(), gfx_height());
}

bool camera_update_viewport(struct camera* c, bool in_water, int width, int height) {
	if(!c || width <= 0 || height <= 0) return false;

	glm_perspective(glm_rad(gstate.config.fov)
						* (in_water ? 6.0F / 7.0F : 1.0F),
					(float)width / (float)height, 0.075F,
					gstate.config.render_distance, c->projection);

	glm_lookat((vec3) {c->x, c->y, c->z},
			   (vec3) {c->x + sinf(c->rx) * sinf(c->ry), c->y + cosf(c->ry),
					   c->z + cosf(c->rx) * sinf(c->ry)},
			   (vec3) {0, 1, 0}, c->view);

	/* Beta VIEW BOBBING [740c583 EntityRenderer.setupCameraTransform:285-299]:
	 * on the GL modelview, hurtCameraEffect then (viewBobbing-gated)
	 * setupViewBobbing run BEFORE orientCamera -- with GL post-multiply
	 * semantics that means the bob translate/rotates act in EYE space on top
	 * of the camera transform, i.e. view = Bob * lookat. Same composition
	 * here. Applied before the frustum planes are derived so culling sees the
	 * bobbed view. bob_active is armed by camera_attach, so the detached free
	 * camera (camera_physics) and menus never take this branch. */
	if(c->bob_active) {
		if(gstate.config.view_bobbing) {
			float t[5];
			camera_bob_terms(&c->bob, c->bob_partial, t);
			mat4 bob;
			glm_mat4_identity(bob);
			/* Order is Beta's exactly: translate, rotate Z, rotate X, rotate X
			 * [740c583 EntityRenderer.setupViewBobbing:178-182]. */
			glm_translate(bob, (vec3) {t[0], t[1], 0.0F});
			glm_rotate(bob, glm_rad(t[2]), (vec3) {0.0F, 0.0F, 1.0F});
			glm_rotate(bob, glm_rad(t[3]), (vec3) {1.0F, 0.0F, 0.0F});
			glm_rotate(bob, glm_rad(t[4]), (vec3) {1.0F, 0.0F, 0.0F});
			glm_mat4_mul(bob, c->view, c->view);
		}

		/* Beta HURT/DEATH CAMERA ROLL [740c583 EntityRenderer
		 * .setupCameraTransform:283-287]: hurtCameraEffect runs BEFORE
		 * setupViewBobbing and is NOT gated by the viewBobbing setting, so it
		 * left-multiplies on top of the bob (view = Hurt * Bob * lookat).
		 * Within hurtCameraEffect the death tilt precedes the hurt kick
		 * [:156-167]; attackedAtYaw's Y-conjugation collapses to the plain Z
		 * roll because our MP-architecture client never learns a damage
		 * direction (hud_hurt.h header comment). */
		float droll = hud_hurt_death_roll_deg(&c->hurt, c->bob_partial);
		float hroll = hud_hurt_camera_roll_deg(&c->hurt, c->bob_partial);
		if(droll != 0.0F || hroll != 0.0F) {
			mat4 h;
			glm_mat4_identity(h);
			if(droll != 0.0F)
				glm_rotate(h, glm_rad(droll), (vec3) {0.0F, 0.0F, 1.0F});
			if(hroll != 0.0F)
				glm_rotate(h, glm_rad(hroll), (vec3) {0.0F, 0.0F, 1.0F});
			glm_mat4_mul(h, c->view, c->view);
		}
	}
	c->bob_active = false;

	mat4 view_proj;
	glm_mat4_mul(c->projection, c->view, view_proj);
	glm_frustum_planes(view_proj, c->frustum_planes);
	return true;
}

void camera_attach(struct camera* c, struct entity* e, float tick_delta,
	float dt) {
	float yaw = c->rx, pitch = c->ry;
	float jdx, jdy;
	if(e->data.local_player.capture_input && input_joystick(dt, &jdx, &jdy)) {
		yaw -= jdx * 2.0F;
		pitch -= jdy * 2.0F;
	}

	// Autopilot authors the look directly (no mouse in a headless capture run).
	if(gstate.autopilot.active) {
		yaw = gstate.autopilot.yaw;
		pitch = gstate.autopilot.pitch;
	}

#ifdef CAVEWII_AGENT_BRIDGE
	// CAVEWII_AGENT bridge authors absolute look (degrees in, same convention the
	// PERCEIVE schema reports). After autopilot so the agent wins if both are enabled;
	// the clamp below still applies.
	{
		float rx = yaw, ry = pitch;
		if(agent_input_apply_look(&rx, &ry)) {
			yaw = rx;
			pitch = ry;
		}
	}
#endif

	/* A2-W: player 0's world is the one its body lives in (its WORLD_RESET
	 * bound it to that slot's terrain) -- not slot 0's storage once player 0
	 * is in another world. Flag OFF / no body: gstate.world, as before. */
	const struct camera_player_view owner = {
		.world = e && e->world ? (struct world*)e->world : &gstate.world,
		.ride = &gstate.players[0].ride,
		.sleeping = gstate.players[0].sleeping, .yaw = yaw, .pitch = pitch,
	};
	camera_attach_owned(c, e, tick_delta, dt, &owner);
}

void camera_attach_owned(struct camera* c, struct entity* e, float tick_delta,
	float dt, const struct camera_player_view* owner) {
	assert(c && e && owner && owner->world);
	vec3 pos_lerp;
	glm_vec3_lerp(e->pos_old, e->pos, tick_delta, pos_lerp);
	c->x = pos_lerp[0];
	c->y = pos_lerp[1];
	c->z = pos_lerp[2];

	/* F4 invariant 8: apply camera-only presentation correction after
	 * ordinary player interpolation, then decay it by rendered-frame dt
	 * (not 20 Hz client ticks). Catch-up ticks cannot consume the
	 * correction before a frame is shown. Converge monotonically to
	 * exact zero over 0.20 seconds.
	 *
	 * Feedback guard: derive C_current = start * remaining
	 * at entry, store AND apply that same vector, then advance elapsed
	 * only. The next vector is derived at the next attach. This keeps
	 * the stored correction synchronized with what was actually applied
	 * to gstate.players[0].camera for the entire inter-frame interval, so
	 * clin_update's wire subtraction (camera - correction) does not feed
	 * a decay step back to the server as false rider motion. */
	if(owner->ride && owner->ride->camera_correction_active) {
		float t = owner->ride->camera_correction_elapsed / 0.20F;
		if(t >= 1.0F) {
			/* Convergence complete: clear on this attach entry so the
			 * stored vector is zero for the inter-frame interval. Do
			 * NOT advance elapsed further. */
			glm_vec3_zero(owner->ride->camera_correction);
			owner->ride->camera_correction_active = false;
		} else {
			float remaining = 1.0F - t;
			/* Derive and store the vector that will be applied this
			 * frame. The stored vector must equal what is applied to
			 * gstate.players[0].camera for the whole inter-frame interval. */
			owner->ride->camera_correction[0]
				= owner->ride->camera_correction_start[0] * remaining;
			owner->ride->camera_correction[1]
				= owner->ride->camera_correction_start[1] * remaining;
			owner->ride->camera_correction[2]
				= owner->ride->camera_correction_start[2] * remaining;
			/* Apply the same stored vector to the camera. */
			c->x += owner->ride->camera_correction[0];
			c->y += owner->ride->camera_correction[1];
			c->z += owner->ride->camera_correction[2];
			/* Advance elapsed only; the next attach derives the next
			 * vector. Do NOT clear here even if this increment crosses
			 * 0.20 -- the stored vector must stay nonzero for the
			 * inter-frame interval until the next attach entry clears
			 * it. */
			owner->ride->camera_correction_elapsed += dt;
		}
	}

	/* Beta SNEAK CAMERA DIP [740c583 Entity.moveEntity:413 -- posY =
	 * boundingBox.minY + yOffset - ySize]. The EYE drops by the interpolated
	 * ySize; the bounding box does not move, because Entity.setPosition:132
	 * puts it back at posY - yOffset + ySize. Applied here, after the
	 * position lerp and the ride correction, so the ORDER the wire fold-out
	 * below assumes holds: the recorded pre/post pair brackets exactly this
	 * one subtraction.
	 *
	 * Same discipline as camera_correction directly above -- record what was
	 * applied so the value baked into the stored camera is known for the
	 * whole inter-frame interval, and clin_update can take it back out of
	 * SRPC_PLAYER_POS. The pair is recorded (not just the dip) because
	 * (a - d) + d is not bit-exact in IEEE-754; see player_ysize.h. */
	c->ysize.applied = player_ysize_lerp(&c->ysize, tick_delta);
	c->ysize.pre_dip_y = c->y;
	c->y -= c->ysize.applied;
	c->ysize.post_dip_y = c->y;

	//printf("c%.03f %.03f %.03f\n", c->x, c->y, c->z);

	c->rx = owner->yaw;
	c->ry = owner->pitch;

	/* Beta IN-BED CAMERA LOCK [740c583 EntityRenderer.orientCamera:192-205].
	 * While sleeping, Beta's branch pre-applies the exact INVERSE of the
	 * standard look tail (yaw+180 about -Y, pitch about -X, against the
	 * tail's +X/+Y) so the player's look input cancels out COMPLETELY, and
	 * what remains is a single rotate of (bedMeta&3)*90 about Y read from the
	 * block at the sleeper's own cell: the view lies flat (pitch 0) at the
	 * pillow, looking along the bed toward its FOOT end, and the mouse does
	 * nothing until wake. Mirrored here on our angles: ry = pi/2 exactly
	 * (horizontal; look.y = cos(ry) = 0) and rx chosen so
	 * (sin rx, 0, cos rx) = -headBlockToFootBlockMap[meta&3] (Beta's own
	 * layout, block/bed_meta.h: {+Z,-X,-Z,+X} foot->head; the occupied bit
	 * 0x4 is ignored exactly as Beta's `var11 & 3`) -- the pillow-to-foot
	 * heading. Raw server chunks carry this layout too, so online beds
	 * orient the same way. Wiimote/stick look is ignored for the duration exactly as
	 * Beta ignored the mouse (a faithful lock, not a playability tweak), so
	 * this runs AFTER the autopilot/agent authors and the position snap
	 * (server_local_begin_sleep) has already parked the eye at the pillow.
	 * If the bed block is gone mid-sleep (broken under the sleeper), keep
	 * the current yaw rather than porting Beta's degenerate identity-view
	 * quirk; the server wakes the player on the next tick anyway. */
	/* Pre-sleep look survives the night: Beta's sleep GUI takes mouse focus,
	 * so rotationYaw/Pitch simply stop changing and the wake-up view is the
	 * pre-sleep view. Our stick keeps feeding c->rx/ry above even while the
	 * lock below overrides them, so save the angles on the falling-asleep
	 * edge and restore them on wake -- per-camera state, client-only. */
	{
		if(owner->sleeping && !c->was_sleeping) {
			c->wake_rx = c->rx;
			c->wake_ry = c->ry;
		} else if(!owner->sleeping && c->was_sleeping) {
			c->rx = c->wake_rx;
			c->ry = c->wake_ry;
		}
		c->was_sleeping = owner->sleeping;
	}
	if(owner->sleeping) {
		struct block_data bed = world_get_block(
			owner->world, (w_coord_t)floorf(c->x), (w_coord_t)floorf(c->y),
			(w_coord_t)floorf(c->z));
		if(bed.type == BLOCK_BED) {
			static const float bed_view_yaw[4]
				= {GLM_PI, GLM_PI_2, 0.0F, -GLM_PI_2};
			c->rx = bed_view_yaw[bed_meta_direction(bed.metadata)];
		}
		c->ry = GLM_PI_2;
	}

	c->ry = glm_clamp(c->ry, glm_rad(0.5F), GLM_PI - glm_rad(0.5F));

	e->orient[0] = c->rx;
	e->orient[1] = c->ry;

	/* Arm view bobbing for this frame's camera_update. tick_delta is Beta's
	 * partial-tick "f" handed to setupViewBobbing. */
	c->bob_partial = tick_delta;
	c->bob_active = true;
}

void camera_bob_client_tick(struct camera* c, const struct entity* player) {
	/* Hurt-feedback state rides the same 20 Hz feed (hud_hurt.h): the health
	 * observed here is the authoritative CRPC_PLAYER_SET_HEALTH value. */
	hud_hurt_tick(&c->hurt, player ? player->health : 0, player != NULL);
	if(player) {
		/* pos - pos_old is this tick's post-collision displacement (Beta's
		 * moveEntity walked-distance input); vel is Beta's motionX/Y/Z at the
		 * camera-sway read (see camera_bob.h for the accepted feel-layer
		 * delta). */
		camera_bob_tick(&c->bob, player->pos[0] - player->pos_old[0],
						player->pos[2] - player->pos_old[2], player->vel[0],
						player->vel[1], player->vel[2], player->on_ground,
						player->health > 0);
	} else {
		camera_bob_tick(&c->bob, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, true, false);
	}
}


void camera_get_ray(const struct camera *c, vec3 origin, vec3 dir) {
    // Set origin to the camera’s current position
    origin[0] = c->x;
    origin[1] = c->y;
    origin[2] = c->z;

    // Compute an endpoint 4.5 units in front of the camera using rx/ry angles
    vec3 end;
    end[0] = c->x + sinf(c->rx) * sinf(c->ry) * 4.5f;
    end[1] = c->y +            cosf(c->ry) * 4.5f;
    end[2] = c->z + cosf(c->rx) * sinf(c->ry) * 4.5f;

    // Subtract origin from end to get the unnormalized direction
    glm_vec3_sub(end, origin, dir);

    // Normalize the direction vector to unit length
    glm_vec3_normalize(dir);
}
