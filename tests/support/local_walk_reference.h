/* Frozen pre-owner walking algorithm; only state/input paths are parameterized.
 * Never update this oracle to follow production changes. The ONE exception is
 * a Beta-cited physics correction applied to both on purpose (87c9029d sneak
 * dip; next-gaps-0924/W1A friction/accel/ladder/no-unstuck, each marked). */
#include "block/block_slipperiness.h"
#include "block/blocks.h"
#include "worldgen/beta_math.h"
#define EYE_HEIGHT 1.62F
static void ref_liquid_aabb(struct AABB* out, struct block_info* blk_info) {
	int block_height = (blk_info->block->metadata & 0x8) ?
		16 :
		(8 - blk_info->block->metadata) * 2 * 7 / 8;
	aabb_setsize(out, 1.0F, (float)block_height / 16.0F, 1.0F);
	aabb_translate(out, blk_info->x, blk_info->y, blk_info->z);
}

static bool ref_test_in_lava(struct AABB* entity, struct block_info* blk_info) {
	if(blk_info->block->type != BLOCK_LAVA_FLOW
	   && blk_info->block->type != BLOCK_LAVA_STILL)
		return false;

	struct AABB bbox;
	ref_liquid_aabb(&bbox, blk_info);
	return aabb_intersection(entity, &bbox);
}

static bool ref_test_in_water(struct AABB* entity, struct block_info* blk_info) {
	if(blk_info->block->type != BLOCK_WATER_FLOW
	   && blk_info->block->type != BLOCK_WATER_STILL)
		return false;

	struct AABB bbox;
	ref_liquid_aabb(&bbox, blk_info);
	return aabb_intersection(entity, &bbox);
}

static bool ref_test_in_liquid(struct AABB* entity, struct block_info* blk_info) {
	return ref_test_in_water(entity, blk_info) || ref_test_in_lava(entity, blk_info);
}

static bool local_walk_reference(struct entity* e,
	struct client_session* session, const struct input_player_frame* frame) {
	assert(e && session);
	/* Beta EntityPlayer.isMovementBlocked returns true while sleeping, and
	 * sleepInBedAt zeros all three motion components [740c583 EntityPlayer].
	 * Do this before minecart controls and ordinary physics: a server-only RPC
	 * gate otherwise lets the client accumulate displacement and submit it on
	 * the first wake frame. Preserve the interpolation samples so render/camera
	 * attachment also sees a stationary body. */
	if(session->sleeping) {
		glm_vec3_copy(e->pos, e->pos_old);
		glm_vec2_copy(e->orient, e->orient_old);
		glm_vec3_zero(e->vel);
		player_ysize_reset(&session->camera.ysize);
		return false;
	}

	// Audit 2026-09-01 #1 (REGRESSION-TODAY, CONFIRMED): reaching this point
	// means the boat search above did NOT find an occupied boat for this
	// entity this tick -- i.e. this player is not boat-mounted right now,
	// whether that is "never mounted" or "just dismounted" (DISMOUNT site:
	// the boat loop stops matching the same tick server_local_set_boat_
	// occupancy(s, boat, false) flips boat->data.boat.occupied). Before this
	// fix, boat_rider_vel_x/z were left holding whatever the LAST mounted
	// tick wrote (or, pre-ctor-fix above, raw malloc garbage), breaking the
	// field's own "0.0 whenever not boat-mounted" contract (entity.h) and
	// transmitting a stale nonzero vel_x/vel_z over every SRPC_PLAYER_POS
	// (client_interface.c ~:3376) for a player who is provably not riding.
	e->data.local_player.boat_rider_vel_x = 0.0F;
	e->data.local_player.boat_rider_vel_z = 0.0F;

	// ---------- normal player physics ----------
	glm_vec3_copy(e->pos, e->pos_old);
	glm_vec2_copy(e->orient, e->orient_old);

	/* W1A-b mirror: no 0.005 motion cutoff (not in Beta 1.7.3). */

	struct AABB bbox;
	aabb_setsize_centered(&bbox, 0.6F, 1.0F, 0.6F);
	aabb_translate(&bbox, e->pos[0], e->pos[1] + 1.8F / 2.0F - EYE_HEIGHT, e->pos[2]);

	bool in_water = entity_intersection(e, &bbox, ref_test_in_water);
	bool in_lava  = entity_intersection(e, &bbox, ref_test_in_lava);

	/* next-gaps-0924/W1A mirror (Beta-cited physics change, not a production
	 * follow): EntityLiving.moveEntityWithHeading:478-497 ground friction and
	 * the 0.02F air/water/lava moveFlying accel -- see entity_local_player.c. */
	float ground_friction = 0.91F;
	if(e->on_ground) {
		ground_friction = 546.0F * 0.1F * 0.1F * 0.1F;
		struct block_data under;
		if(entity_get_block(e, (w_coord_t)floorf(e->pos[0]),
							(w_coord_t)floorf(e->pos[1] - EYE_HEIGHT) - 1,
							(w_coord_t)floorf(e->pos[2]), &under)
		   && under.type > 0)
			ground_friction = block_slipperiness(under.type) * 0.91F;
	}
	float move_accel;
	if(in_water || in_lava)
		move_accel = 0.02F;
	else if(e->on_ground)
		move_accel = 0.1F
			* (0.16277136F
			   / (ground_friction * ground_friction * ground_friction));
	else
		move_accel = 0.02F;

	float forward = 0.0F;
	float strafe  = 0.0F;
	bool jumping  = false;
	bool sneaking = false;

	if(e->data.local_player.capture_input) {
		if((frame && frame->connected && frame->down[IB_FORWARD]))  forward += 1.0F;
		if((frame && frame->connected && frame->down[IB_BACKWARD])) forward -= 1.0F;
		if((frame && frame->connected && frame->down[IB_RIGHT]))    strafe  += 1.0F;
		if((frame && frame->connected && frame->down[IB_LEFT]))     strafe  -= 1.0F;
		jumping  = (frame && frame->connected && frame->down[IB_JUMP]);
		sneaking = (frame && frame->connected && frame->down[IB_SNEAK]);
	}

	/* Beta SNEAK CAMERA DIP, step 1 of 2 [740c583 EntityPlayerSP.onLivingUpdate:79-80],
	 * mirroring entity_local_player_walk_owned. Camera only -- it moves nothing
	 * this reference computes, but the shadow must stay byte-identical in the
	 * SESSION too, which is what test_bed_camera memcmps. */
	player_ysize_pre_move(&session->camera.ysize, sneaking);

	/* W1A-b mirror: EntityPlayerSP.pushOutOfBlocks x4 (onLivingUpdate:83-86). */
	for(int corner = 0; corner < 4; corner++) {
		static const int sxs[4] = {-1, -1, 1, 1}, szs[4] = {1, -1, -1, 1};
		double px = (double)e->pos[0] + (double)sxs[corner] * (double)0.6F * 0.35;
		double py = (double)(e->pos[1] - EYE_HEIGHT) + 0.5;
		double pz = (double)e->pos[2] + (double)szs[corner] * (double)0.6F * 0.35;
		w_coord_t bx = (w_coord_t)floor(px), by = (w_coord_t)floor(py),
				  bz = (w_coord_t)floor(pz);
		double fx = px - (double)bx, fz = pz - (double)bz;
#define REF_CUBE(X, Y, Z)                                                        \
	(entity_get_block(e, (X), (Y), (Z), &ref_blk)                              \
	 && block_is_normal_cube(ref_blk.type))
		struct block_data ref_blk;
		if(!REF_CUBE(bx, by, bz) && !REF_CUBE(bx, by + 1, bz))
			continue;
		bool onx = !REF_CUBE(bx - 1, by, bz) && !REF_CUBE(bx - 1, by + 1, bz);
		bool opx = !REF_CUBE(bx + 1, by, bz) && !REF_CUBE(bx + 1, by + 1, bz);
		bool onz = !REF_CUBE(bx, by, bz - 1) && !REF_CUBE(bx, by + 1, bz - 1);
		bool opz = !REF_CUBE(bx, by, bz + 1) && !REF_CUBE(bx, by + 1, bz + 1);
#undef REF_CUBE
		int dir = -1;
		double best = 9999.0;
		if(onx && fx < best) { best = fx; dir = 0; }
		if(opx && 1.0 - fx < best) { best = 1.0 - fx; dir = 1; }
		if(onz && fz < best) { best = fz; dir = 4; }
		if(opz && 1.0 - fz < best) { best = 1.0 - fz; dir = 5; }
		if(dir == 0) e->vel[0] = -0.1F;
		if(dir == 1) e->vel[0] = 0.1F;
		if(dir == 4) e->vel[2] = -0.1F;
		if(dir == 5) e->vel[2] = 0.1F;
	}

	// Beta 1.7.3 sneak speed (MovementInputFromOptions): moveStrafe/moveForward are scaled
	// by 0.3 BEFORE moveEntityWithHeading's max(sqrt(s^2+f^2),1) normalization. Our distf
	// normalization below matches Beta's clamp, so applying 0.3 to forward/strafe here is
	// byte-faithful -- and identical to the old int code for the non-sneak case (1.0 == 1).
	if(sneaking) {
		forward *= 0.3F;
		strafe  *= 0.3F;
	}

	/* W1A mirror: EntityLiving.onLivingUpdate:638-640 input *= 0.98F, then
	 * Entity.moveFlying:570-585. */
	forward *= 0.98F;
	strafe  *= 0.98F;
	float mag = sqrtf(strafe * strafe + forward * forward);
	if(mag >= 0.01F) {
		if(mag < 1.0F)
			mag = 1.0F;
		mag = move_accel / mag;
		/* W1A-b mirror: MathHelper table, Beta yaw = -deg(orient[0]),
		 * Beta moveStrafe = -strafe. */
		float s = -strafe * mag;
		float f = forward * mag;
		float yaw = -glm_deg(e->orient[0]);
		float angle = yaw * (float)M_PI / 180.0F;
		float sn = mh_sin(angle);
		float cs = mh_cos(angle);
		e->vel[0] += s * cs - f * sn;
		e->vel[2] += f * cs + s * sn;
	}

	/* W1A-b mirror: no jump cooldown (EntityLiving.onLivingUpdate:625-634). */
	if(jumping) {
		if(in_water || in_lava)
			e->vel[1] += 0.04F;
		else if(e->on_ground)
			e->vel[1] = 0.42F;
	}

	aabb_setsize_centered(&bbox, 0.6F, 1.8F, 0.6F);
	aabb_translate(&bbox, 0.0F, 1.8F / 2.0F - EYE_HEIGHT, 0.0F);

	/* W1A mirror: no unstuck lift (Beta has none); the pre-move ladder clamp
	 * and sneak hold of EntityLiving.moveEntityWithHeading:500-527. */
	if(!in_water && !in_lava) {
		struct block_data rung;
		if(entity_get_block(e, (w_coord_t)floorf(e->pos[0]),
							(w_coord_t)floorf(e->pos[1] - EYE_HEIGHT),
							(w_coord_t)floorf(e->pos[2]), &rung)
		   && rung.type == BLOCK_LADDER) {
			const float rail = 0.15F;
			if(e->vel[0] < -rail) e->vel[0] = -rail;
			if(e->vel[0] > rail)  e->vel[0] = rail;
			if(e->vel[2] < -rail) e->vel[2] = -rail;
			if(e->vel[2] > rail)  e->vel[2] = rail;
			if((double)e->vel[1] < -0.15)
				e->vel[1] = (float)-0.15;
			if(sneaking && e->vel[1] < 0.0F)
				e->vel[1] = 0.0F;
		}
	}

	// Beta 1.7.3 sneak ledge-stop (Entity.moveEntity, onGround && isSneaking): clamp the
	// horizontal move so the player can't step off an edge. Done after the unstuck nudge
	// and BEFORE the new_pos/new_vel copies + the collision move, so the step-up path below
	// sees the clamped velocity too.
	// The same pre-move snapshot is Beta's `var18` footstep suppression, read
	// by the MP footstep block after the move [740c583: Entity.moveEntity:478].
	bool step_suppress = sneaking && e->on_ground;
	if(step_suppress)
		entity_sneak_ledge_clamp(e, &bbox);

	/* Beta SNEAK CAMERA DIP, step 2 of 2 [740c583 Entity.moveEntity:259]. */
	player_ysize_move(&session->camera.ysize);

	vec3 new_pos, new_vel;
	glm_vec3_copy(e->pos, new_pos);
	glm_vec3_copy(e->vel, new_vel);

	bool collision_xz = false;
	for(int k = 0; k < 3; k++)
		entity_try_move(e, e->pos, e->vel, &bbox, (size_t[]){1, 0, 2}[k],
		                &collision_xz, &e->on_ground);

	if(e->on_ground) {
		bool collision = false;
		bool ground    = e->on_ground;

		new_vel[1] = 0.6F;
		entity_try_move(e, new_pos, new_vel, &bbox, 1, &collision, &ground);

		new_vel[1] = 0.0F;
		entity_try_move(e, new_pos, new_vel, &bbox, 0, &collision, &ground);
		entity_try_move(e, new_pos, new_vel, &bbox, 2, &collision, &ground);

		new_vel[1] = -0.6F;
		entity_try_move(e, new_pos, new_vel, &bbox, 1, &collision, &ground);

		if(new_pos[1] > e->pos_old[1]
		   && glm_vec3_distance2(e->pos_old, e->pos) < glm_vec3_distance2(e->pos_old, new_pos)) {
			collision_xz = collision;
			e->on_ground = ground;
			glm_vec3_copy(new_pos, e->pos);
			glm_vec3_copy(new_vel, e->vel);
		}
	}

	// Beta Entity.moveEntity block-collide dispatch (soul sand motionX/Z *= 0.4 per
	// intersected cell) runs at the END of the move, i.e. after the collision moves
	// above and BEFORE the friction below -- Beta's exact order. The 7/8 soul-sand
	// collision box is what puts the standing player's feet inside the cell.
	// [decompile 740c583 Entity.moveEntity + BlockSoulSand]
	{
		struct AABB sbox = bbox; // relative box (already carries the eye-height offset)
		aabb_translate(&sbox, e->pos[0], e->pos[1], e->pos[2]);
		entity_soulsand_slowdown(e, &sbox, &e->vel[0], &e->vel[2]);
	}

	// MP client-side footstep (remote_session only -- in SP server_local plays
	// the identical step from the SRPC_PLAYER_POS delta, server_local.c, and
	// this gate keeps the two paths exclusive). Beta's step block runs right
	// here inside moveEntity, after the collision move: gate
	// `canTriggerWalking() && !var18 && ridingEntity == null` (var18 = the
	// pre-move onGround && isSneaking snapshot above; the riding early-returns
	// never reach this point), accumulate sqrt(dx^2+dz^2)*0.6 into the
	// odometer, and on the nextStepDistance crossing with a non-air block
	// under the feet play stepSoundDir2() at getVolume()*0.15F / getPitch()
	// with the snow-layer-above override, at the FEET. The 2-block teleport
	// cap mirrors the SP handler's documented deviation (corrections/respawn
	// teleports must not accumulate). [740c583: Entity.moveEntity:478-499]
	if(gstate.remote_session && !step_suppress) {
		double ddx = (double)e->pos[0] - (double)e->pos_old[0];
		double ddz = (double)e->pos[2] - (double)e->pos_old[2];
		if(ddx * ddx + ddz * ddz < 4.0) {
			int bx = (int)floorf(e->pos[0]);
			int by = (int)floorf(e->pos[1] - 0.2F - EYE_HEIGHT);
			int bz = (int)floorf(e->pos[2]);
			struct block_data foot, above;
			bool has_foot = entity_get_block(e, bx, by, bz, &foot);
			bool solid = has_foot && foot.type != BLOCK_AIR;
			if(server_player_walk_step(&e->data.local_player.walk_dist,
									   &e->data.local_player.walk_next_step,
									   ddx, ddz, solid)
			   && has_foot) {
				uint8_t above_id
					= entity_get_block(e, bx, by + 1, bz, &above)
					? (uint8_t)above.type
					: (uint8_t)BLOCK_AIR;
				float vol, pitch;
				uint16_t sid = audio_footstep_params((uint8_t)foot.type,
													 above_id, &vol, &pitch);
				if(sid != AUDIO_SOUND_NONE)
					audio_play3d(sid,
								 (vec3) {e->pos[0], e->pos[1] - EYE_HEIGHT,
										 e->pos[2]},
								 vol, pitch);
			}
		}
	}

	/* W1A mirror: Beta tests isInWater() before handleLavaMovement(); land
	 * branch per EntityLiving.moveEntityWithHeading:528-535. */
	if(in_water) {
		e->vel[0] *= 0.8F;
		e->vel[2] *= 0.8F;
		e->vel[1]  = e->vel[1] * 0.8F - 0.02F;
	} else if(in_lava) {
		e->vel[0] *= 0.5F;
		e->vel[2] *= 0.5F;
		e->vel[1]  = e->vel[1] * 0.5F - 0.02F;
	} else {
		struct block_data blk;
		if(collision_xz
		   && entity_get_block(e, (w_coord_t)floorf(e->pos[0]),
							   (w_coord_t)floorf(e->pos[1] - EYE_HEIGHT),
							   (w_coord_t)floorf(e->pos[2]), &blk)
		   && blk.type == BLOCK_LADDER)
			e->vel[1] = (float)0.2;
		e->vel[1] -= 0.08F;
		e->vel[1] *= 0.98F;
		e->vel[0] = (float)((double)e->vel[0] * (double)ground_friction);
		e->vel[2] = (float)((double)e->vel[2] * (double)ground_friction);
	}

	if(collision_xz && (in_lava || in_water)) {
		struct AABB tmp;
		aabb_setsize_centered(&tmp, 0.6F, 1.8F, 0.6F);
		aabb_translate(&tmp, e->pos[0] + e->vel[0],
		               e->pos[1] + e->vel[1] + 1.8F / 2.0F - 1.62F + 0.6F,
		               e->pos[2] + e->vel[2]);
		if(!entity_intersection(e, &tmp, ref_test_in_liquid))
			e->vel[1] = 0.3F;
	}

	// update client-side oxygen bar
	if(session->in_water) {
		session->oxygen--;
		// water_demo showcase: hold the air bar near full so the Beta bubble HUD
		// stays on-screen for screenshots (normal gameplay drains it fully).
		if(gstate.water_demo && session->oxygen < MAX_OXYGEN - 32 * 3)
			session->oxygen = MAX_OXYGEN - 32 * 3;
	} else session->oxygen = MAX_OXYGEN;

	return false;
}
#undef EYE_HEIGHT
