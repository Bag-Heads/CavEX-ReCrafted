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

#ifndef GAME_CAMERA_BOB_H
#define GAME_CAMERA_BOB_H

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846 // strict C99 (glibc/devkitPPC) hides it
#endif
#include <stdbool.h>

/* Beta 1.7.3 first-person VIEW BOBBING, render/feel layer only (no simulation
 * state). Ported from [740c583 EntityRenderer.setupViewBobbing:171-184] with
 * its per-tick inputs from [740c583 EntityPlayer.onLivingUpdate:178-195],
 * [740c583 EntityLiving:142] (prevCameraPitch roll) and
 * [740c583 Entity.moveEntity:199,479] (distanceWalkedModified). Beta gates the
 * whole effect on GameSettings.viewBobbing, default TRUE, saved as "bobView"
 * [740c583 GameSettings:21,331]; ours is gstate.config.view_bobbing
 * ("render.view_bobbing", default on) applied in camera_update.
 *
 * Kept as a pure header (combat.h precedent) so test_view_bobbing can pin the
 * constants without linking the client. All angles produced are DEGREES, like
 * Beta's glRotatef arguments; the caller converts.
 *
 * Known, accepted delta (feel layer, not simulation): Beta's cameraYaw "f"
 * term reads motionX/motionZ AFTER the move+friction of the same tick; we read
 * the entity velocity our client tick left behind. Our client player is
 * server-authoritative with client prediction, so the exact Beta float stream
 * is unreachable here by construction -- the shape, caps, gates and smoothing
 * constants below are what define the Beta feel and are exact. */

struct camera_bob_state {
	/* distanceWalkedModified + prev [740c583 Entity:40,199] */
	float dwm, dwm_prev;
	/* cameraYaw/cameraPitch + prevs [740c583 EntityPlayer:145-146,194-195] */
	float cam_yaw, cam_yaw_prev;
	float cam_pitch, cam_pitch_prev;
};

/* Once per 20 Hz client tick, after the local player entity ticked.
 * move_dx/move_dz: the ACTUAL horizontal displacement this tick
 * (pos - pos_old; Beta adds the post-clip move [740c583 Entity.moveEntity:479]).
 * vel_*: entity velocity in blocks/tick (Beta motionX/Y/Z). */
static inline void camera_bob_tick(struct camera_bob_state* b, float move_dx,
								   float move_dz, float vel_x, float vel_y,
								   float vel_z, bool on_ground, bool alive) {
	/* prev rolls happen before the new values, once per tick
	 * [740c583 EntityPlayer:178 (yaw), EntityLiving:142 (pitch),
	 *  Entity.moveEntity:199 (walked distance)] */
	b->dwm_prev = b->dwm;
	b->cam_yaw_prev = b->cam_yaw;
	b->cam_pitch_prev = b->cam_pitch;

	/* distanceWalkedModified += sqrt(dx*dx + dz*dz) * 0.6
	 * [740c583 Entity.moveEntity:479] */
	b->dwm += sqrtf(move_dx * move_dx + move_dz * move_dz) * 0.6F;

	/* [740c583 EntityPlayer.onLivingUpdate:180-195]:
	 *   f  = sqrt(motionX^2 + motionZ^2), capped at 0.1,
	 *        zeroed airborne or dead;
	 *   f1 = atan(-motionY * 0.2) * 15, zeroed grounded or dead;
	 *   cameraYaw   += (f  - cameraYaw)   * 0.4
	 *   cameraPitch += (f1 - cameraPitch) * 0.8   (0.8, NOT 0.4) */
	float f = sqrtf(vel_x * vel_x + vel_z * vel_z);
	float f1 = atanf(-vel_y * 0.2F) * 15.0F;
	if(f > 0.1F)
		f = 0.1F;
	if(!on_ground || !alive)
		f = 0.0F;
	if(on_ground || !alive)
		f1 = 0.0F;
	b->cam_yaw += (f - b->cam_yaw) * 0.4F;
	b->cam_pitch += (f1 - b->cam_pitch) * 0.8F;
}

/* The five eye-space transform terms of [740c583
 * EntityRenderer.setupViewBobbing:174-182], for partial tick `partial`,
 * IN BETA'S APPLICATION ORDER:
 *   out[0] tx      = sin(f2*pi) * yaw * 0.5      (glTranslatef x)
 *   out[1] ty      = -|cos(f2*pi) * yaw|          (glTranslatef y)
 *   out[2] rz_deg  = sin(f2*pi) * yaw * 3         (glRotatef about Z)
 *   out[3] rxa_deg = |cos(f2*pi - 0.2) * yaw| * 5 (glRotatef about X)
 *   out[4] rxb_deg = pitch (interpolated cameraPitch, glRotatef about X)
 * where f2 = -(dwm + (dwm - dwm_prev) * partial) and yaw/pitch are the
 * partial-tick lerps of cameraYaw/cameraPitch. */
static inline void camera_bob_terms(const struct camera_bob_state* b,
									float partial, float out[5]) {
	float walk = b->dwm - b->dwm_prev;
	float f2 = -(b->dwm + walk * partial);
	float yaw = b->cam_yaw_prev + (b->cam_yaw - b->cam_yaw_prev) * partial;
	float pitch
		= b->cam_pitch_prev + (b->cam_pitch - b->cam_pitch_prev) * partial;
	out[0] = sinf(f2 * (float)M_PI) * yaw * 0.5F;
	out[1] = -fabsf(cosf(f2 * (float)M_PI) * yaw);
	out[2] = sinf(f2 * (float)M_PI) * yaw * 3.0F;
	out[3] = fabsf(cosf(f2 * (float)M_PI - 0.2F) * yaw) * 5.0F;
	out[4] = pitch;
}

#endif
