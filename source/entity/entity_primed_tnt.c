/*
	Copyright (c) 2026 CaveWii_v2 contributors

	This file is part of CavEX.

	CavEX is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.
*/

#include <assert.h>
#include <math.h>

#include "../block/aabb.h"
#include "../block/blocks_data.h"
#include "../graphics/render_item.h"
#include "../item/items.h"
#include "../network/client_interface.h"
#include "../network/server_local.h"
#include "../network/server_world.h"
#include "../particle.h"
#include "../platform/gfx.h"
#include "entity.h"
#include "entity_primed_tnt.h"

#define PRIMED_TNT_GRAVITY 0.04f
#define PRIMED_TNT_DRAG 0.98f
#define PRIMED_TNT_GROUND_DRAG 0.7f
#define PRIMED_TNT_GROUND_BOUNCE -0.5f
#define PRIMED_TNT_TRACKER_INTERVAL 10

static void primed_tnt_bbox(const struct entity* e, struct AABB* out) {
	entity_make_local_bbox(e, out);
}

static size_t get_bounding_box(const struct entity* e, struct AABB* out) {
	assert(e && out);
	entity_make_world_bbox(e, out);
	return 1;
}

bool primed_tnt_advance(struct entity* e) {
	assert(e);
	glm_vec3_copy(e->pos, e->pos_old);

	/*
		EntityTNTPrimed.onUpdate exact order: gravity, moveEntity, all-axis
		drag, grounded drag/bounce, then the post-decrement fuse test.
	*/
	e->vel[1] -= PRIMED_TNT_GRAVITY;
	struct AABB bbox;
	primed_tnt_bbox(e, &bbox);
	bool collision_xz = false;
	entity_try_move(e, e->pos, e->vel, &bbox, 1, &collision_xz,
					&e->on_ground);
	entity_try_move(e, e->pos, e->vel, &bbox, 0, &collision_xz,
					&e->on_ground);
	entity_try_move(e, e->pos, e->vel, &bbox, 2, &collision_xz,
					&e->on_ground);

	e->vel[0] *= PRIMED_TNT_DRAG;
	e->vel[1] *= PRIMED_TNT_DRAG;
	e->vel[2] *= PRIMED_TNT_DRAG;
	if(e->on_ground) {
		e->vel[0] *= PRIMED_TNT_GROUND_DRAG;
		e->vel[2] *= PRIMED_TNT_GROUND_DRAG;
		e->vel[1] *= PRIMED_TNT_GROUND_BOUNCE;
	}

	return primed_tnt_fuse_update_should_explode(
		&e->data.primed_tnt.fuse);
}

static bool entity_client_tick(struct entity* e) {
	assert(e);
	/* The server owns removal. Normally DESTROY follows EXPLODE immediately;
	 * under the Wii cap a fuse-zero server entity may instead be retained for a
	 * retry. Do not let a locally predicted expiry delete the only visible copy
	 * before that authoritative decision arrives. */
	if(e->data.primed_tnt.awaiting_destroy)
		return false;
	bool expired = primed_tnt_advance(e);
	if(expired) {
		e->data.primed_tnt.fuse = 0;
		e->data.primed_tnt.awaiting_destroy = true;
		return false;
	}
	particle_generate_tnt_smoke(e->pos);
	return false;
}

static bool primed_tnt_send_correction(struct entity* e) {
	assert(e && e->on_server && e->type == ENTITY_PRIMED_TNT);
	struct client_rpc call = {
		.type = CRPC_PRIMED_TNT_CORRECTION,
		.payload.primed_tnt_correction = {
			.entity_id = e->id,
			.pos = {e->pos[0], e->pos[1], e->pos[2]},
			.vel = {e->vel[0], e->vel[1], e->vel[2]},
			.fuse = (int8_t)e->data.primed_tnt.fuse,
		},
	};
	if(!clin_try_rpc_send(&call))
		return false;
	e->data.primed_tnt.correction_ticks = 0;
	return true;
}

static bool entity_server_step(struct entity* e, struct server_local* s) {
	assert(e && s);
	/* The Wii cap is not allowed to turn Beta's synchronous explode-before-dead
	 * edge into a lost entity. Snapshot the fuse-zero motion step so a saturated
	 * admission window can leave the durable source byte-for-byte ready to retry
	 * at the same detonation position next update. In the ordinary admitted path
	 * the exact EntityTNTPrimed.onUpdate order above is unchanged.
	 * [decompile 740c583 EntityTNTPrimed.onUpdate + explode] */
	vec3 pos_before, pos_old_before, vel_before;
	bool on_ground_before = e->on_ground;
	glm_vec3_copy(e->pos, pos_before);
	glm_vec3_copy(e->pos_old, pos_old_before);
	glm_vec3_copy(e->vel, vel_before);
	if(primed_tnt_advance(e)) {
		vec3 center = {e->pos[0], e->pos[1], e->pos[2]};
		if(server_world_try_explode((struct server_world*)e->world, center, PRIMED_TNT_EXPLOSION_POWER)) {
			e->data.primed_tnt.explosion_pending = false;
			return true;
		}
		glm_vec3_copy(pos_before, e->pos);
		glm_vec3_copy(pos_old_before, e->pos_old);
		glm_vec3_copy(vel_before, e->vel);
		e->on_ground = on_ground_before;
		e->data.primed_tnt.fuse = 0;
		e->data.primed_tnt.explosion_pending = true;
		/* The client may already have predicted expiry. Retry this correction
		 * non-blockingly until the render thread drains a token. */
		e->data.primed_tnt.correction_ticks = PRIMED_TNT_TRACKER_INTERVAL;
		(void)primed_tnt_send_correction(e);
		return false;
	}
	/* Beta EntityTrackerEntry registers TNT at update interval 10 with velocity
	 * tracking enabled (mc173 EntityKind::Tnt => 160,10,true). Local physics
	 * remains smooth; this only corrects accumulated position/fuse drift. */
	if(e->data.primed_tnt.correction_ticks < PRIMED_TNT_TRACKER_INTERVAL)
		e->data.primed_tnt.correction_ticks++;
	if(e->data.primed_tnt.correction_ticks >= PRIMED_TNT_TRACKER_INTERVAL)
		(void)primed_tnt_send_correction(e);
	return false;
}

static bool entity_server_tick(struct entity* e, struct server_local* s) {
	assert(e && s);
	/* More than the cap may be retained. Later owners still reached by the
	 * ordinary entity pass must retry without another physics step or any other
	 * state change. */
	if(entity_primed_tnt_has_pending_explosion(e))
		return entity_primed_tnt_retry_pending_explosion(e, s);
	return entity_server_step(e, s);
}

bool entity_primed_tnt_has_pending_explosion(const struct entity* e) {
	return e && e->type == ENTITY_PRIMED_TNT
		&& e->data.primed_tnt.fuse == 0
		&& e->data.primed_tnt.explosion_pending;
}

bool entity_primed_tnt_retry_pending_explosion(
	struct entity* e, struct server_local* s) {
	assert(entity_primed_tnt_has_pending_explosion(e) && s);
	return entity_server_step(e, s);
}

static void entity_render(struct entity* e, mat4 view, float tick_delta) {
	assert(e);
	struct item_data stack = {
		.id = BLOCK_TNT,
		.durability = 0,
		.count = 1,
	};
	struct item* item = item_get(&stack);
	if(!item || !item->renderItem)
		return;

	vec3 pos;
	glm_vec3_lerp(e->pos_old, e->pos, tick_delta, pos);
	struct block_data light = {0};
	(void)entity_get_block(e, (w_coord_t)floorf(pos[0]),
						 (w_coord_t)floorf(pos[1]),
						 (w_coord_t)floorf(pos[2]), &light);
	render_item_update_light((light.torch_light << 4) | light.sky_light);

	mat4 model, modelview;
	glm_translate_make(model, pos);
	glm_scale_uni(model,
				  primed_tnt_render_scale(e->data.primed_tnt.fuse,
									  tick_delta));
	glm_translate(model, (vec3) {-0.5f, -0.5f, -0.5f});
	glm_mat4_mul(view, model, modelview);

	float flash
		= primed_tnt_flash_amount(e->data.primed_tnt.fuse, tick_delta);
	gfx_entity_tint(1.0f, 1.0f, 1.0f, flash);
	item->renderItem(item, &stack, modelview, false, R_ITEM_ENV_ENTITY);
	/* Mandatory GX TEV-state restore before the next entity/world draw. */
	gfx_entity_tint(0.0f, 0.0f, 0.0f, 0.0f);
}

void entity_primed_tnt(uint32_t id, struct entity* e, bool server, void* world,
					   int fuse) {
	assert(e && world);
	entity_default_init(e, server, world);
	/* EntityTNTPrimed: setSize(.98,.98), yOffset=height/2. */
	entity_set_size(e, PRIMED_TNT_SIZE, PRIMED_TNT_SIZE,
		PRIMED_TNT_SIZE * 0.5F);
	e->id = id;
	e->type = ENTITY_PRIMED_TNT;
	e->name = "Primed TNT";
	e->tick_server = entity_server_tick;
	e->tick_client = entity_client_tick;
	e->render = entity_render;
	e->teleport = entity_default_teleport;
	e->getBoundingBox = get_bounding_box;
	e->onLeftClick = NULL;
	e->leftClickText = NULL;
	e->onRightClick = NULL;
	e->rightClickText = NULL;
	e->data.primed_tnt.fuse = fuse;
	e->data.primed_tnt.explosion_pending = false;
	e->data.primed_tnt.correction_ticks = 0;
	e->data.primed_tnt.awaiting_destroy = false;
	e->on_ground = false;
	glm_vec3_copy(e->pos, e->network_pos);
}
