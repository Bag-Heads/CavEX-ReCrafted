/*
	Copyright (c) 2026 CaveWii_v2 contributors

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

#include "../block/blocks.h" /* block_world: the dispatch (entity tick) world */
#include <assert.h>
#include <math.h>

#include "../block/aabb.h"
#include "../block/block_sand.h"
#include "../block/blocks_data.h"
#include "../graphics/render_item.h"
#include "../item/items.h"
#include "../network/client_interface.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_world.h"
#include "../platform/gfx.h"
#include "entity.h"
#include "entity_falling_sand.h"

static size_t get_bounding_box(const struct entity* e, struct AABB* out) {
	assert(e && out);
	entity_make_world_bbox(e, out);
	return 1;
}

/* Beta EntityFallingSand.onUpdate's shared motion step: gravity, moveEntity
 * (Y, X, Z -- Entity.moveEntity's own axis order, matching
 * entity_primed_tnt.c's primed_tnt_advance precedent, since both entities
 * share the exact same Beta method), then a flat drag on all three axes.
 * Unlike TNT there is no on-ground drag/bounce HERE -- Beta only applies that
 * in the landing branch (entity_server_tick/entity_client_tick below), using
 * FALLING_SAND_GROUND_DRAG/BOUNCE. Does not touch the world (no set_block):
 * the origin-cell clear and the landing placement are both world-mutating
 * side effects that only the SERVER's tick may perform -- the CLIENT's local
 * copy runs this same physics purely to predict smooth motion between the
 * server's CRPC_SET_BLOCK/CRPC_ENTITY_DESTROY, exactly like entity_item.c's
 * client tick never touches blocks either. [decompile 740c583
 * EntityFallingSand.onUpdate] */
static void falling_sand_advance(struct entity* e) {
	assert(e);
	glm_vec3_copy(e->pos, e->pos_old);

	e->vel[1] -= FALLING_SAND_GRAVITY;
	struct AABB bbox;
	entity_make_local_bbox(e, &bbox);
	bool collision_xz = false;
	entity_try_move(e, e->pos, e->vel, &bbox, 1, &collision_xz, &e->on_ground);
	entity_try_move(e, e->pos, e->vel, &bbox, 0, &collision_xz, &e->on_ground);
	entity_try_move(e, e->pos, e->vel, &bbox, 2, &collision_xz, &e->on_ground);

	e->vel[0] *= FALLING_SAND_DRAG;
	e->vel[1] *= FALLING_SAND_DRAG;
	e->vel[2] *= FALLING_SAND_DRAG;
}

/* Beta's onUpdate leads with `if (this.blockID == 0) { this.setEntityDead();
 * } else { ... }`. Omitted here (not silently dropped): block_id is set once
 * at construction to BLOCK_SAND or BLOCK_GRAVEL by every caller
 * (block_sand_try_to_fall only ever passes info->block->type from a live
 * sand/gravel cell) and never mutated afterward, so the guard is provably
 * unreachable on every path this port can construct -- unlike Beta, where a
 * SUBCLASS could theoretically be built with id 0 via the no-arg
 * constructor, a path this port's own entity_falling_sand() constructor
 * signature does not expose. */
static bool entity_client_tick(struct entity* e) {
	assert(e);
	/* The server owns removal. A locally predicted landing must not delete
	 * the only visible copy before the server's authoritative
	 * CRPC_ENTITY_DESTROY (+ CRPC_SET_BLOCK for the settled block) arrives --
	 * entity_primed_tnt.c's awaiting_destroy precedent. */
	if(e->data.falling_sand.awaiting_destroy)
		return false;

	e->data.falling_sand.fall_time++;
	falling_sand_advance(e);

	if(e->on_ground) {
		/* Beta's landing branch motion mutation (dead-ish -- the entity is
		 * destroyed this same tick server-side -- but the client renders one
		 * more interpolated frame before the DESTROY arrives, so transcribe
		 * it rather than skip it). [decompile 740c583
		 * EntityFallingSand.onUpdate] */
		e->vel[0] *= FALLING_SAND_GROUND_DRAG;
		e->vel[2] *= FALLING_SAND_GROUND_DRAG;
		e->vel[1] *= FALLING_SAND_GROUND_BOUNCE;
		e->data.falling_sand.awaiting_destroy = true;
	}
	return false;
}

/* Beta `Entity.dropItem(blockID, 1)` -> `entityDropItem` -> a plain,
 * stationary (no velocity, no throw-RNG) EntityItem at the faller's own
 * position, with delayBeforeCanPickup explicitly set to 10 afterward.
 * [decompile 740c583 Entity.dropItem, Entity.entityDropItem] */
static void falling_sand_drop_item(struct entity* e, struct server_local* s) {
	/* RetroMCP b1.7.3 EntityFallingSand.java:62-67 -> Entity.java:840-844:
	 * failed placement/timeout joins the item in the faller's worldObj.
	 * Resolve its lifetime stamp; never drop into a successor world. */
	struct server_world* w = server_local_entity_world(s, e);
	if(!w)
		return;
	struct item_data drop = {
		.id = e->data.falling_sand.block_id,
		.durability = 0,
		.count = 1,
	};
	vec3 pos = {e->pos[0], e->pos[1], e->pos[2]};
	struct entity* dropped = w == &s->worlds[0]
		? server_local_spawn_item(pos, &drop, false, s)
		: server_local_spawn_item_w(w, pos, &drop, false);
	if(dropped)
		dropped->data.item.pickup_delay = 10;
}

static bool entity_server_tick(struct entity* e, struct server_local* s) {
	assert(e && s);
	e->data.falling_sand.fall_time++;
	falling_sand_advance(e);

	w_coord_t fx = (w_coord_t)floorf(e->pos[0]);
	w_coord_t fy = (w_coord_t)floorf(e->pos[1]);
	w_coord_t fz = (w_coord_t)floorf(e->pos[2]);

	/* `if (worldObj.getBlockId(x,y,z) == blockID) setBlockWithNotify(x,y,z,0)`
	 * -- runs EVERY tick (not just the first), but the original sand/gravel
	 * cell is never explicitly cleared before this entity spawns (BlockSand.
	 * tryToFall leaves it in place), so this is where -- and when -- the
	 * source block actually disappears: the entity's own first update tick,
	 * once its (still nearly unmoved) floor position still reads back the
	 * carried id. [decompile 740c583 EntityFallingSand.onUpdate] */
	struct block_data here;
	if(server_world_get_block(block_world(s), fx, fy, fz, &here)
	   && here.type == e->data.falling_sand.block_id)
		server_world_set_block(
			(struct server_world*)e->world, fx, fy, fz, (struct block_data) {.type = BLOCK_AIR, .metadata = 0});

	if(e->on_ground) {
		e->vel[0] *= FALLING_SAND_GROUND_DRAG;
		e->vel[2] *= FALLING_SAND_GROUND_DRAG;
		e->vel[1] *= FALLING_SAND_GROUND_BOUNCE;

		/* Beta: `if ((!canBlockBePlacedAt || canFallBelow(below) ||
		 * !setBlockWithNotify) && !multiplayerWorld) dropItem(...)`. A
		 * short-circuit OR: canBlockBePlacedAt is checked FIRST (failing it
		 * skips the placement attempt entirely); only if it passes is
		 * canFallBelow(below) checked (support just below the landing cell --
		 * if there's none, it would immediately fall again, so skip placing
		 * and drop instead); only if BOTH pass does the actual
		 * setBlockWithNotify placement run. This function only ever executes
		 * server-side, so "!multiplayerWorld" is unconditionally true here
		 * (the same reasoning entity_item.c's tick_server drops rely on).
		 * [decompile 740c583 EntityFallingSand.onUpdate] */
		bool placed = block_sand_can_place_at(s, fx, fy, fz)
			&& !block_sand_can_fall_below(s, fx, fy - 1, fz)
			&& server_world_set_block(
				   (struct server_world*)e->world, fx, fy, fz,
				   (struct block_data) {
					   .type = e->data.falling_sand.block_id, .metadata = 0});
		if(!placed)
			falling_sand_drop_item(e, s);
		return true; // setEntityDead()
	} else if(e->data.falling_sand.fall_time
			  > FALLING_SAND_VOID_GIVE_UP_TICKS) {
		falling_sand_drop_item(e, s);
		return true;
	}
	return false;
}

/* TNT-precedent moving-block render (entity_primed_tnt.c's entity_render):
 * draw the carried block id as a full item-rendered cube at the entity's
 * interpolated position, no tint/scale effects (Beta's own RenderFallingSand
 * draws the block model at a flat scale of 1 with no fuse-style pulse). Both
 * block_sand's and block_gravel's block_item.renderItem are render_item_block
 * (the same full-cube item renderer TNT's render reuses via BLOCK_TNT), so
 * carrying an arbitrary block id through `stack.id` is a direct
 * generalisation of that exact precedent -- no new assets needed. */
static void entity_render(struct entity* e, mat4 view, float tick_delta) {
	assert(e);
	struct item_data stack = {
		.id = e->data.falling_sand.block_id,
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
						 (w_coord_t)floorf(pos[1]), (w_coord_t)floorf(pos[2]),
						 &light);
	render_item_update_light((light.torch_light << 4) | light.sky_light);

	mat4 model, modelview;
	glm_translate_make(model, pos);
	glm_translate(model, (vec3) {-0.5f, -0.5f, -0.5f});
	glm_mat4_mul(view, model, modelview);
	item->renderItem(item, &stack, modelview, false, R_ITEM_ENV_ENTITY);
}

void entity_falling_sand(uint32_t id, struct entity* e, bool server,
						 void* world, uint8_t block_id) {
	assert(e && world);
	entity_default_init(e, server, world);
	/* EntityFallingSand: setSize(.98,.98), yOffset=height/2. */
	entity_set_size(e, FALLING_SAND_SIZE, FALLING_SAND_SIZE,
		FALLING_SAND_SIZE * 0.5f);
	e->id = id;
	e->type = ENTITY_FALLING_SAND;
	e->name = "Falling Sand";
	e->tick_server = entity_server_tick;
	e->tick_client = entity_client_tick;
	e->render = entity_render;
	e->teleport = entity_default_teleport;
	e->getBoundingBox = get_bounding_box;
	e->onLeftClick = NULL;
	e->leftClickText = NULL;
	e->onRightClick = NULL;
	e->rightClickText = NULL;
	e->data.falling_sand.block_id = block_id;
	e->data.falling_sand.fall_time = 0;
	e->data.falling_sand.awaiting_destroy = false;
	e->on_ground = false;
	glm_vec3_copy(e->pos, e->network_pos);
}
