/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

/* Soft-snap melee assist -- a Wii-controller playability carve-out
 * (docs/deviations.md "Soft-snap melee assist",
 * docs/wii_controller_playability.md #3; committee decision
 * tmp/jobs/aim-help-committee/DECISION.md, 2026-10-05).
 *
 * attack_pick() is the per-pane crosshair pick main.c runs every frame. With
 * the assist Off it is Beta's EntityRenderer.getMouseOver (critique-1028):
 * the 4.0 block ray first, then the nearest canBeCollidedWith entity (grown
 * by its collision border) within min(block hit, 3.0); own body ignored.
 * tests/test_attack_assist.c pins it. With Low/Normal it additionally, and ONLY
 * when that pick missed everything (no entity, no block), looks for one
 * eligible mob inside a 3/6 degree half-angle cone around the look ray:
 *   - the same 3.0 entity reach, the mob's own unchanged hitbox (no growth);
 *   - the corrected ray must really intersect that hitbox, and neither a
 *     block nor another entity may stand in front of it (occlusion);
 *   - eligible = living hostile mobs, an angry wild wolf, an angry pigman;
 *     never players, passive animals, tamed wolves, items, vehicles,
 *     paintings, projectiles;
 *   - ranking: smallest angle, then distance, then entity id;
 *   - never on a remote server (the effective cone is 0 there).
 * The result goes to camera_ray_result.assist_hit/assist_entity_id, which
 * only the left-click attack and the crosshair tint read. No camera movement,
 * no auto-swing, no cadence change; the server keeps revalidating reach and
 * canEntityBeSeen on SRPC_ENTITY_ATTACK exactly as before. */

#ifndef ATTACK_ASSIST_H
#define ATTACK_ASSIST_H

#include <stdbool.h>
#include <stdint.h>

#include "../entity/entity.h"
#include "camera.h"

enum attack_assist_level {
	ATTACK_ASSIST_OFF = 0,
	ATTACK_ASSIST_LOW = 1,	  /* 3 degree half-angle */
	ATTACK_ASSIST_NORMAL = 2, /* 6 degree half-angle */
	ATTACK_ASSIST_LEVELS = 3,
};

/* The block ray length: Beta survival getBlockReachDistance() = 4.0F
 * [b1.7.3 PlayerControllerSP.java:109-111, PlayerControllerMP.java:118-120],
 * cast from the eye by EntityLiving.rayTrace (EntityLiving.java:835-840).
 * critique-1028: was 4.5. The servers stay looser (dig d^2 > 36 / place
 * d^2 >= 64 refused), so every 4.0 pick is accepted. */
#define ATTACK_PICK_REACH 4.0F
/* Entity reach: Beta EntityRenderer.getMouseOver clamps the entity ray to
 * min(block-hit distance, 3.0) in survival [b1.7.3 EntityRenderer.java:85-99]
 * (critique-1028; was 4.5 through raycast_entity). Applies to the direct pick
 * AND the soft-snap assist (the assist never reaches past the direct pick). */
#define ATTACK_ENTITY_REACH 3.0F

/* Effective cone half-angle in degrees for a setting level: 0 when the level
 * is Off or out of range, and ALWAYS 0 on a remote server. */
float attack_assist_half_angle_deg(int level, bool remote_session);

/* Target eligibility only (no geometry): living hostile mob, angry wild
 * wolf, angry pigman. Dead (death_time > 0), destroyed (delay_destroy == 0)
 * and id 0 are never eligible. */
bool attack_assist_eligible(const struct entity* e);

struct attack_pick_input {
	dict_entity_t* entities;	/* this pane's client entity view */
	const struct entity* self;	/* this pane's own local player (ignored) */
	const struct camera* camera; /* this pane's camera -- the only one read */
	camera_ray_cell_fn cell;	/* block test (world_block_intersection) */
	void* cell_user;
	int assist_level;			/* enum attack_assist_level */
	bool remote_session;
	/* Local split-screen only: the OTHER pane's local player body, or NULL.
	 * raycast_entity skips every ENTITY_LOCAL_PLAYER (and the host body's id
	 * 0), so without this the peer can never be punched. Beta's pick excludes
	 * only the viewer itself [b1.7.3 EntityRenderer.java:105
	 * getEntitiesWithinAABBExcludingEntity(this.mc.renderViewEntity, ...)],
	 * so the peer is a direct target AND occludes entities behind it. It is
	 * considered only while it is in `entities` under its own id (same
	 * world view) and alive (health > 0: the server refuses dead targets,
	 * server_local_attack_split_player). NULL (every remote session and
	 * every single-player frame) = the pre-peer pick, byte for byte. */
	const struct entity* peer;
};

/* Writes the pick into *res. Fields the pre-assist pick left untouched stay
 * untouched (x/y/z/side on an entity hit or a block miss). assist_hit and
 * assist_entity_id are always written. */
void attack_pick(const struct attack_pick_input* in,
				 struct camera_ray_result* res);

/* Crosshair feedback (committee ruling: needed to judge the feel). The HUD
 * draws the crosshair in this solid colour instead of MODE_INVERT exactly
 * when an attack this frame would land on the assisted target. */
#define ATTACK_ASSIST_TINT_R 0xFF
#define ATTACK_ASSIST_TINT_G 0x50
#define ATTACK_ASSIST_TINT_B 0x40
static inline bool
attack_assist_crosshair_tinted(const struct camera_ray_result* r) {
	return r && r->assist_hit && !r->hit && !r->entity_hit;
}

/* Production wrapper: the block test is world_block_intersection on w. */
void attack_pick_world(dict_entity_t* entities, const struct entity* self,
					   const struct entity* peer,
					   const struct camera* camera, struct world* w,
					   int assist_level, bool remote_session,
					   struct camera_ray_result* res);

#endif
