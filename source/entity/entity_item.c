/*
	Copyright (c) 2023 ByteBit/xtreme8000

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

#include "../block/block_pressure_plate.h" // audit #24: block_pressure_plate_on_entity_tick
#include "../block/block_slipperiness.h" // next-gaps-0924/W1C: EntityItem ground friction
#include "../block/blocks.h" // block_is_normal_cube (Entity.pushOutOfBlocks)
#include "../block/blocks_data.h"
#include "../game/game_state.h"
#include "../network/client_interface.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "../network/server_weather.h" // server_entity_is_wet (Entity.moveEntity burn tail)
#include "../network/server_world.h" // server_world_aabb_has_lava (handleLavaMovement)
#include "../platform/audio.h" /* S2: SOUND_RANDOM_POP pickup pop */
#include "../platform/gfx.h"
#include "entity.h"
#include "entity_pickup_flight.h" // MP visual-parity pass item 4: pure flight-lerp math
#include "../graphics/gfx_settings.h"
#include "../graphics/render_item.h" // direct include (was transitive via blocks.h pre-split)

/* S2: Beta EntityItem.onCollideWithPlayer's pickup pop -- "random.pop" 0.2F,
   pitch ((r-r)*0.7F+1.0F)*2.0F, at the item. Beta plays it whenever
   addItemStackToInventory accepted anything, so BOTH the full collect and
   the partial-absorb branch pop (the entity survives a partial with the
   remainder). Pitch from the dedicated sound stream (cosmetic-RNG rule).
   [740c583: EntityItem.onCollideWithPlayer] */
static void item_emit_pickup_pop(struct entity* e) {
	struct random_gen* g = server_local_sound_rng();
	server_local_emit_sound(
		SOUND_RANDOM_POP, (vec3) {e->pos[0], e->pos[1], e->pos[2]}, 0.2F,
		((rand_gen_flt(g) - rand_gen_flt(g)) * 0.7F + 1.0F) * 2.0F);
}

/* Beta EntityItem.onUpdate ground factor [EntityItem.java:55-62]:
 * The airborne factor starts at 0.98F.
 * On ground, the fallback factor multiplies 0.1F, 0.1F and 58.8F.
 * Read the block below the floored box minimum Y, at floored X and Z.
 * A non-air block supplies its slipperiness multiplied by 0.98F.
 * Only the on-ground half lives here (callers keep the 0.98F airborne arm).
 * slipperiness is Block.java:143 0.6F / BlockIce.java:8 0.98F. The air-below
 * constant 0.1F*0.1F*58.8F rounds to the very float 0.6F*0.98F does (both
 * 0x3f16872c), so the branch is kept for transcription fidelity, not for an
 * observable difference. An unloaded cell reads as air, like Beta's
 * getBlockId on a missing chunk. */
static bool entity_item_handle_water_movement(struct entity* e);

static float entity_item_ground_friction(struct entity* e) {
	struct AABB box;
	entity_make_world_bbox(e, &box);
	struct block_data below;
	if(entity_get_block(e, (w_coord_t)floorf(e->pos[0]),
						(w_coord_t)floorf(box.y1) - 1,
						(w_coord_t)floorf(e->pos[2]), &below)
	   && below.type != BLOCK_AIR)
		return block_slipperiness(below.type) * 0.98F;
	return 0.1F * 0.1F * 58.8F;
}

static bool entity_client_tick(struct entity* e) {
	assert(e);

	// MP visual-parity pass item 4: while a pickup flight is in progress the
	// item is Beta-frozen (EntityPickupFX's entityToPickUp was removed from
	// world, so its own onUpdate/physics never run again -- see entity.h's
	// pickup_flight_* field comment). Suspend physics/aging entirely and just
	// advance the flight's own age counter; entity_render computes the
	// lerped flight position independently of pos/pos_old below. Returning
	// true signals entities_client_tick (entity.c) to free+erase this entity
	// the instant the flight completes, exactly ENTITY_ITEM_PICKUP_FLIGHT_TICKS
	// ticks after CRPC_PICKUP_ITEM armed it.
	if(e->data.item.pickup_flight_active) {
		e->data.item.pickup_flight_age++;
		return e->data.item.pickup_flight_age >= ENTITY_ITEM_PICKUP_FLIGHT_TICKS;
	}

	/* next-gaps-0924/W1C-b: the client predicts every item tick (the server
	 * sends no per-tick item moves), so it runs the SAME Beta order as
	 * entity_server_tick [EntityItem.java:45-69]: gravity, move, ground-aware
	 * friction, landing bounce -- no CavEX |v| < 0.005 snap and no CavEX
	 * inclusive "stuck" probe. The two steps that draw the entity Random
	 * (lava bounce :46-51, pushOutOfBlocks :53) are server-only; the server
	 * sends CRPC_ENTITY_PHYSICS_CORRECTION with its post-tick pos/vel on
	 * every tick either one fires, so the prediction is re-seated there. */
	glm_vec3_copy(e->pos, e->pos_old);
	glm_vec2_copy(e->orient, e->orient_old);

	/* next-gaps-0924/W2 P3: the water-flow impulse is part of that same Beta
	 * order (super.onUpdate() -> handleWaterMovement, before gravity), so the
	 * prediction applies it too, from the client world; the server tick is
	 * the authority and its verdict-side effects (fire, splash) stay there. */
	(void)entity_item_handle_water_movement(e);

	e->vel[1] -= 0.04F;

	/* next-gaps-0924/W2 P12: the same isInWeb branch and post-move cobweb
	 * sweep as the server tick, so the prediction matches (entity.h). */
	const bool web_move = entity_web_consume(e);
	struct AABB bbox;
	entity_make_local_bbox(e, &bbox);
	bool collision_xz = false;
	for(int k = 0; k < 3; k++)
		entity_try_move(e, e->pos, e->vel, &bbox, (size_t[]) {1, 0, 2}[k],
						&collision_xz, &e->on_ground);
	if(web_move)
		glm_vec3_zero(e->vel);
	{
		struct AABB wbox;
		entity_make_world_bbox(e, &wbox);
		entity_web_contact(e, &wbox);
	}

	float f = e->on_ground ? entity_item_ground_friction(e) : 0.98F;
	e->vel[0] *= f;
	e->vel[1] *= 0.98F;
	e->vel[2] *= f;
	if(e->on_ground)
		e->vel[1] *= -0.5F;

	e->data.item.age++;
	return false;
}

static size_t getBoundingBox(const struct entity *e, struct AABB *out) {
    assert(e && out);
	entity_make_world_bbox(e, out);
    return 1;
}

// Audit #24: Beta EntityItem.onUpdate calls Entity.moveEntity (unlike
// EntityArrow, which never does -- see block_pressure_plate.h), so a dropped
// item DOES reach BlockPressurePlate.onEntityCollidedWithBlock every tick its
// post-move box overlaps a plate's cell -- and since a wood plate registers
// EnumMobType.everything (no entity-type filter at all), an item is exactly
// the case that must press a wood plate while NOT pressing a stone one
// (stone is EnumMobType.mobs == EntityLiving.class only). Same +/-0.001
// inclusive per-cell scan shape as entity_monster.c's post-move cactus scan
// (a shared extraction is the CODESTYLE 3.2 candidate, deferred as its own
// refactor commit rather than bundled here -- see that function's comment).
// [740c583: Entity.moveEntity, BlockPressurePlate.onEntityCollidedWithBlock]
//
// Cactus rides the SAME sweep, for the same reason: BlockCactus
// .onEntityCollidedWithBlock deals one damage point to the entity
// [740c583: BlockCactus.java:129-131], and EntityItem OVERRIDES
// attackEntityFrom to record the hit, subtract damage, and die at zero
// health; the constructor supplies five health points
// [740c583: EntityItem.java:22,43 and :108-116]. So a stack resting against a
// cactus dies after exactly five contact ticks (there is no hurtResistantTime
// on EntityItem -- that field is EntityLiving's), where the base
// Entity.attackEntityFrom every non-living kind inherits is a no-op
// records the hit and returns false [740c583: Entity.java:770-774].
//
// Which OTHER kinds Beta reaches with this sweep, checked against the
// decompile rather than assumed: EntityArrow, EntityEgg, EntitySnowball and
// EntityFireball never call moveEntity at all (they integrate position with
// setPosition), so a cactus can never touch them -- nothing to add there.
// EntityFallingSand and EntityTNTPrimed do call moveEntity but inherit the
// no-op base attackEntityFrom, so the sweep is observably inert for them.
// EntityBoat/EntityMinecart/EntityPainting do react in Beta, but none of those
// kinds runs this post-move sweep in this port at all yet -- a pre-existing,
// separate gap, named here so it is not mistaken for a silent omission of
// this fix.
// Returns true when this tick's contact killed the item (Beta setEntityDead).
static bool entity_item_post_move_block_collisions(struct entity* e,
													struct server_local* s) {
	assert(e && s);

	struct AABB box;
	entity_make_world_bbox(e, &box);
	w_coord_t x0 = (w_coord_t)floorf(box.x1 + 0.001F);
	w_coord_t y0 = (w_coord_t)floorf(box.y1 + 0.001F);
	w_coord_t z0 = (w_coord_t)floorf(box.z1 + 0.001F);
	w_coord_t x1 = (w_coord_t)floorf(box.x2 - 0.001F);
	w_coord_t y1 = (w_coord_t)floorf(box.y2 - 0.001F);
	w_coord_t z1 = (w_coord_t)floorf(box.z2 - 0.001F);

	if(y1 < 0 || y0 >= WORLD_HEIGHT)
		return false;

	w_coord_t cx0 = WCOORD_CHUNK_OFFSET(x0);
	w_coord_t cz0 = WCOORD_CHUNK_OFFSET(z0);
	w_coord_t cx1 = WCOORD_CHUNK_OFFSET(x1);
	w_coord_t cz1 = WCOORD_CHUNK_OFFSET(z1);
	/* Entity.moveEntity: checkChunksExist belongs to this entity's World,
	 * just like the block reads in the sweep below. */
	struct server_world* w = server_local_entity_world(s, e);
	if(!w)
		return false;
	for(w_coord_t cx = cx0; cx <= cx1; cx++)
		for(w_coord_t cz = cz0; cz <= cz1; cz++)
			if(!dict_server_chunks_get(w->chunks, S_CHUNK_ID(cx, cz)))
				return false;

	bool dead = false;
	for(w_coord_t x = x0; x <= x1; x++)
		for(w_coord_t y = y0; y <= y1; y++)
			for(w_coord_t z = z0; z <= z1; z++) {
				struct block_data blk;
				if(!server_world_get_block(block_world(s), x, y, z, &blk))
					continue;
				if(blk.type == BLOCK_STONE_PRESSURE_PLATE
				   || blk.type == BLOCK_WOOD_PRESSURE_PLATE)
					block_pressure_plate_on_entity_tick(s, x, y, z);
				/* next-gaps-0924/W2 P12: [BlockWeb.java:10-12] isInWeb. */
				if(blk.type == BLOCK_COBWEB)
					e->in_web = true;
				/* EntityItem.attackEntityFrom(null, 1): one point per
				 * OVERLAPPED CELL per tick, exactly like Beta -- a stack wedged
				 * into a corner of two cactus cells really does take two hits
				 * in one tick, because the sweep calls the hook per cell.
				 * [740c583: BlockCactus.java:129-131 -> EntityItem.java:108-116] */
				if(blk.type == BLOCK_CACTUS) {
					e->health -= 1;
					if(e->health <= 0)
						dead = true;
				}
			}
	return dead;
}

/* EntityItem.onUpdate: age 6000 triggers death (5 min). */
#define ENTITY_ITEM_DESPAWN_AGE (5 * 60 * 20)

/* Beta asks each living player for item entities intersecting that player's
 * boundingBox.expand(1, 0, 1), skipping isDead ones, then calls
 * EntityItem.onCollideWithPlayer. ONE item-local predicate for BOTH actors
 * (host and guest): the host used a sphere `distance2 < 2^2` around
 * (x, y - 0.6, z) until itement-fix-0925, which disagreed with Beta at the
 * box's X/Z edges (a sphere hit past the expanded edge) and at its top
 * corners (a box hit outside the sphere). server_nearest_player and every
 * mob/AI caller are unchanged. The Y extent is the player box itself:
 * posY is the eye, minY = posY - 1.62, height 1.8 [Entity.setPosition].
 * [740c583: EntityPlayer.java:196-203; AxisAlignedBB.java:185] */
static bool entity_item_player_overlap(const struct entity* e,
									   const struct server_player* player) {
	struct AABB player_box;
	aabb_setsize_centered(&player_box, 0.6F, 1.8F, 0.6F);
	aabb_translate(&player_box, (float)player->x,
				   (float)player->y + 0.9F - 1.62F, (float)player->z);
	player_box.x1 -= 1.0F;
	player_box.x2 += 1.0F;
	player_box.z1 -= 1.0F;
	player_box.z2 += 1.0F;

	struct AABB item_box;
	entity_make_world_bbox(e, &item_box);
	return aabb_intersection_strict(&player_box, &item_box);
}

static void entity_item_try_guest_collect(struct entity* e,
									  struct server_local* s) {
	struct server_player* guest = s->guest_actor;
	if(!guest || !s->guest_gameplay || !guest->finished_loading
	   || server_transfer_actor_inert(s, guest)
	   || !server_local_entity_world(s, e)
	   || server_local_entity_world(s, e) != server_local_world_of(s, guest)
	   || guest->health <= 0 || !entity_item_player_overlap(e, guest))
		return;

	bool collected = false;
	uint8_t count_before = e->data.item.item.count;
	struct inventory* inv = &guest->inventory;
	if(inv->logic && inv->logic->on_collect)
		collected = inv->logic->on_collect(inv, guest, &e->data.item.item);

	/* EntityItem kills only after the acting inventory accepted the complete
	 * stack. EntityPlayerMP identifies that same actor in Packet22Collect.
	 * [740c583: EntityItem.java:110-130; EntityPlayerMP.java:206-220] */
	if(collected) {
		item_emit_pickup_pop(e);
		server_local_send_entity(s, e, (struct client_rpc) {
			.type = CRPC_PICKUP_ITEM,
			.payload.pickup_item.entity_id = e->id,
			.payload.pickup_item.collector_id = s->guest_entity_id,
		});
		e->delay_destroy = 1;
	} else if(e->data.item.item.count != count_before) {
		/* Partial fill: addItemStackToInventory returned false, so Beta plays
		 * no pop; only the remainder count is mirrored (host arm below).
		 * [InventoryPlayer.java:143-149; EntityItem.java:113,122] */
		server_local_send_entity(s, e, (struct client_rpc) {
			.type = CRPC_ITEM_COUNT,
			.payload.item_count.entity_id = e->id,
			.payload.item_count.count = e->data.item.item.count,
		});
	}
}

/* Beta's per-entity `rand` (Entity.java:41 `new Random()`), never
 * World.rand: every EntityItem gameplay roll (lava bounce, pushOutOfBlocks)
 * draws from the item's OWN java.util.Random-exact gameplay_rng, seeded once
 * per server lifetime in entity_item() (entity_lifetime_rng_seed). This keeps
 * s->rand_src aligned to Beta's world-stream draw count (CODESTYLE 2.3) and
 * keeps one item's rolls from shifting another's. */

/* The SP client predicts items itself (entity_client_tick); a server step
 * that used the entity Random re-seats it with the server's post-tick state
 * (the redstone_piston_server.c pmove_send_entity_correction precedent;
 * clin_apply_entity_physics_correction's generic arm covers ENTITY_ITEM). */
static void entity_item_send_correction(struct server_local* s, const struct entity* e) {
	server_local_send_entity(s, e, (struct client_rpc) {
		.type = CRPC_ENTITY_PHYSICS_CORRECTION,
		.payload.entity_physics_correction = {
			.entity_id = e->id,
			.pos = {e->pos[0], e->pos[1], e->pos[2]},
			.vel = {e->vel[0], e->vel[1], e->vel[2]},
			.y_size = e->y_size,
			.fall_distance = e->fall_distance,
			.on_ground = e->on_ground,
			.collided_horizontally = e->collided_horizontally,
			.collided_vertically = e->collided_vertically,
			.in_web = e->in_web,
			.on_fire = e->on_fire,
		},
	});
}

/* ---- next-gaps-0924/W1C: the Entity/EntityItem environment hooks an item
 * reaches every server tick. All damage lands on the item's own 5-point
 * health (EntityItem.java:8) with no hurt-resistance window. */

/* EntityItem.attackEntityFrom [EntityItem.java:87-95]: subtract damage;
 * nonpositive health causes death, which is read by the caller. */
static void entity_item_attack(struct entity* e, int dmg) {
	e->health -= dmg;
}

/* EntityItem.handleWaterMovement [EntityItem.java:79-81]: Beta's ONE
 * override -- World.handleMaterialAcceleration over the PLAIN boundingBox
 * (no -0.4F shrink, no 0.001 contraction), i.e. the Entity.setPosition box
 * posX +- 0.125F, minY = posY - yOffset 0.125F + ySize, + 0.25F. Returns the
 * inWater verdict (fire quench, isWet, the splash latch) and adds the 0.014D
 * flow impulse to e->vel -- the FIRST thing EntityItem.onUpdate does through
 * super.onUpdate(), before gravity and the move. next-gaps-0924/W2 P3
 * (docs/intent/p3-water-flow-push.md): replaces the W1C foot-cell probe. */
static bool entity_item_handle_water_movement(struct entity* e) {
	const double half = (double)(e->width / 2.0F);
	const double min_y
		= (double)e->pos[1] - (double)e->y_offset + (double)e->y_size;
	return entity_water_acceleration(
		e, (double)e->pos[0] - half, min_y, (double)e->pos[2] - half,
		(double)e->pos[0] + half, min_y + (double)e->height,
		(double)e->pos[2] + half);
}

/* Entity.handleLavaMovement [Entity.java:218; World.isMaterialInBB]:
 * boundingBox.expand(-0.1F, -0.4F, -0.1F). On a 0.25-tall item the Y range
 * INVERTS (minY + 0.4 > maxY - 0.4), so isMaterialInBB's
 * floor(minY') .. floor(maxY' + 1) half-open scan sees lava only when
 * frac(bb.minY) lies in [0.15, 0.6) -- a faithful Beta quirk the golden's
 * two lava cases pin (death on tick 1 vs tick 2). */
static bool entity_item_handle_lava_movement(struct server_local* s,
											 struct entity* e) {
	struct AABB box;
	entity_make_world_bbox(e, &box);
	return server_world_aabb_has_lava(
		block_world(s), (double)box.x1 + (double)0.1F, (double)box.y1 + (double)0.4F,
		(double)box.z1 + (double)0.1F, (double)box.x2 - (double)0.1F,
		(double)box.y2 - (double)0.4F, (double)box.z2 - (double)0.1F);
}

/* World.isBoundingBoxBurning(boundingBox.contract(0.001)) [Entity.java:481;
 * World.java:1395-1416]: fire OR either lava id over floor(min) ..
 * floor(max + 1.0) EXCLUSIVE, behind checkChunksExist's inclusive chunk span
 * of those same bounds (see server_local_player_fire_contact_for for the
 * seam detail; that player scan is fire-only by a recorded deviation, which
 * does not apply to items -- Beta's item path has no separate lava arbiter,
 * so the full Beta id set is used here). */
#ifndef CAVEWII_SERVER_LOCAL_TEST
static
#endif
bool entity_item_bounding_box_burning(struct server_local* s,
											 struct entity* e) {
	struct AABB box;
	entity_make_world_bbox(e, &box);
	const w_coord_t x0 = (w_coord_t)floor((double)box.x1 + 0.001);
	const w_coord_t x1 = (w_coord_t)floor((double)box.x2 - 0.001 + 1.0);
	const w_coord_t y0 = (w_coord_t)floor((double)box.y1 + 0.001);
	const w_coord_t y1 = (w_coord_t)floor((double)box.y2 - 0.001 + 1.0);
	const w_coord_t z0 = (w_coord_t)floor((double)box.z1 + 0.001);
	const w_coord_t z1 = (w_coord_t)floor((double)box.z2 - 0.001 + 1.0);
	if(y1 < 0 || y0 >= WORLD_HEIGHT)
		return false;
	/* T1 (W1C-b): when Beta's inclusive preflight span is ONE chunk, every
	 * scanned cell lies in that chunk, and server_world_get_block already
	 * answers false for all of them if it is absent -- the preflight cannot
	 * change the result, so it is skipped (0 dict lookups on most item
	 * positions). A multi-chunk span still runs it, through the cached
	 * resolver get_block uses. Proven equal to the dict-scan original by
	 * test_item_env_parity_golden section 8. */
	const w_coord_t cx0 = WCOORD_CHUNK_OFFSET(x0), cx1 = WCOORD_CHUNK_OFFSET(x1);
	const w_coord_t cz0 = WCOORD_CHUNK_OFFSET(z0), cz1 = WCOORD_CHUNK_OFFSET(z1);
	if(cx0 != cx1 || cz0 != cz1)
		for(w_coord_t cx = cx0; cx <= cx1; cx++)
			for(w_coord_t cz = cz0; cz <= cz1; cz++)
				if(!server_world_chunk_resident(block_world(s), cx, cz))
					return false;
	for(w_coord_t x = x0; x < x1; x++)
		for(w_coord_t y = y0; y < y1; y++)
			for(w_coord_t z = z0; z < z1; z++) {
				struct block_data b;
				if(server_world_get_block(block_world(s), x, y, z, &b)
				   && (b.type == BLOCK_FIRE || b.type == BLOCK_LAVA_FLOW
					   || b.type == BLOCK_LAVA_STILL))
					return true;
			}
	return false;
}

static bool entity_item_normal_cube(struct server_local* s, w_coord_t x,
									w_coord_t y, w_coord_t z) {
	struct block_data b;
	return server_world_get_block(block_world(s), x, y, z, &b)
		&& block_is_normal_cube(b.type);
}

/* Entity.pushOutOfBlocks(posX, (bb.minY + bb.maxY) / 2, posZ)
 * [Entity.java:1056-1129; EntityItem.java:53]. Replaces the CavEX
 * "is item stuck" probe, whose INCLUSIVE box test counted an item resting
 * exactly on a floor face (Beta's normal resting state, minY == block top)
 * as stuck and shoved it sideways every tick. Beta acts only when the box
 * CENTER cell is a normal cube: pick the nearest open face (ties keep the
 * earlier side: -X, +X, -Y, +Y, -Z, +Z) and set that motion axis to
 * +/-(rand.nextFloat() * 0.2F + 0.1F). The roll is drawn whenever the
 * center is embedded, even with no open face (var22 == -1). Entity rand ->
 * the item's gameplay_rng, as for the lava bounce. Returns true when an axis was
 * set (the client correction trigger). */
static bool entity_item_push_out_of_blocks(struct server_local* s,
										   struct entity* e) {
	struct AABB box;
	entity_make_world_bbox(e, &box);
	const double px = e->pos[0];
	const double py = ((double)box.y1 + (double)box.y2) / 2.0;
	const double pz = e->pos[2];
	const w_coord_t x = (w_coord_t)floor(px);
	const w_coord_t y = (w_coord_t)floor(py);
	const w_coord_t z = (w_coord_t)floor(pz);
	if(!entity_item_normal_cube(s, x, y, z))
		return false;
	const double fx = px - (double)x, fy = py - (double)y, fz = pz - (double)z;
	const bool open[6] = {
		!entity_item_normal_cube(s, x - 1, y, z),
		!entity_item_normal_cube(s, x + 1, y, z),
		!entity_item_normal_cube(s, x, y - 1, z),
		!entity_item_normal_cube(s, x, y + 1, z),
		!entity_item_normal_cube(s, x, y, z - 1),
		!entity_item_normal_cube(s, x, y, z + 1),
	};
	const double dist[6] = {fx, 1.0 - fx, fy, 1.0 - fy, fz, 1.0 - fz};
	int side = -1;
	double best = 9999.0;
	for(int k = 0; k < 6; k++)
		if(open[k] && dist[k] < best) {
			best = dist[k];
			side = k;
		}
	float push = jrand_next_float(&e->gameplay_rng) * 0.2F + 0.1F;
	if(side >= 0)
		e->vel[side / 2] = (side % 2) ? push : -push;
	return side >= 0;
}

static bool entity_server_tick(struct entity* e, struct server_local* s) {
	assert(e);

	/* EntityItem.attackEntityFrom (an explosion's entity pass, run from
	 * another entity's tick): subtract damage and die at nonpositive health;
	 * the dead item is skipped by onUpdate and removed
	 * at the end of the world's entity loop. The blast cannot erase the dict
	 * entry mid-iteration, so the removal lands here, on the item's own tick.
	 * [740c583 EntityItem.attackEntityFrom:87-93] */
	if(e->health <= 0)
		return true;

	/* ---- super.onUpdate() -> Entity.onEntityUpdate, the item-reachable
	 * part [Entity.java:194-220]. EntityItem is not isImmuneToFire, has no
	 * hurt-resistance, and takes every hit through attackEntityFrom
	 * [EntityItem.java:83-95]. A hit that kills here (setEntityDead) does NOT
	 * stop the rest of EntityItem.onUpdate -- Java keeps executing it (the
	 * lava bounce still draws its RNG, the move still runs); the entity is
	 * only removed after its onUpdate returns, which is the `return true`
	 * below the physics. */
	int16_t* fire = &e->data.item.persisted_fire;
	/* handleWaterMovement [Entity.java:168]: Beta's box verdict + the flow
	 * impulse (next-gaps-0924/W2 P3), then the dry->wet splash edge at this
	 * same point of the tick [Entity.java:169-190], `fire = 0` [:196]. */
	bool in_water = entity_item_handle_water_movement(e);
	server_local_splash_edge(s, e, in_water, &e->data.item.was_in_water);
	if(in_water)
		*fire = 0; // handleWaterMovement clears fire [Entity.java:196]
	if(*fire > 0) { // [Entity.java:203-216], !isImmuneToFire arm
		if(*fire % 20 == 0)
			entity_item_attack(e, 1);
		--*fire;
	}
	if(entity_item_handle_lava_movement(s, e)) { // [Entity.java:218-240]
		entity_item_attack(e, 4); // setOnFireFromLava
		*fire = 600;
	}
	/* An entity below Y -64 is killed [Entity.java:222-224] -- read before the
	 * move, like Beta; the rest of the tick still runs. */
	bool void_dead = (double)e->pos[1] < -64.0;

	// FH-12: Beta EntityItem.onUpdate's FIRST statement after super.onUpdate()
	// -- an unconditional per-tick decrement, independent of movement/pickup.
	// [740c583: EntityItem.onUpdate:37-39]
	if(e->data.item.pickup_delay > 0)
		e->data.item.pickup_delay--;

	glm_vec3_copy(e->pos, e->pos_old);
	glm_vec2_copy(e->orient, e->orient_old);

	/* ---- EntityItem.onUpdate physics, Beta's order verbatim
	 * [EntityItem.java:45-69]: gravity BEFORE the move (the old CavEX order
	 * applied it after, so every airborne tick moved 0.04 too far and the
	 * trajectory never matched), lava bounce, pushOutOfBlocks, moveEntity,
	 * ground-aware friction, landing "bounce". The CavEX |v| < 0.005 snap to
	 * zero is gone: Beta's EntityItem has none (that is EntityLiving's
	 * onLivingUpdate), and it cut an ice slide short. */
	e->vel[1] -= 0.04F; // motionY -= (double)0.04F [:45]
	bool correct_client = false;

	struct block_data at;
	if(server_world_get_block(block_world(s), (w_coord_t)floorf(e->pos[0]),
							  (w_coord_t)floorf(e->pos[1]),
							  (w_coord_t)floorf(e->pos[2]), &at)
	   && (at.type == BLOCK_LAVA_FLOW || at.type == BLOCK_LAVA_STILL)) {
		/* [EntityItem.java:46-51]. The motion rolls are simulation state on
		 * Beta's per-entity Random (e->gameplay_rng, never the world stream),
		 * drawn in source order through named locals (CODESTYLE 2.3). The fizz
		 * pitch is cosmetic and comes from the dedicated sound stream (the
		 * pickup-pop precedent): 4 entity draws + 1 sound draw, Beta's 5. */
		struct java_random* er = &e->gameplay_rng;
		float r1 = jrand_next_float(er);
		float r2 = jrand_next_float(er);
		float r3 = jrand_next_float(er);
		float r4 = jrand_next_float(er);
		correct_client = true;
		e->vel[1] = 0.2F;
		e->vel[0] = (r1 - r2) * 0.2F;
		e->vel[2] = (r3 - r4) * 0.2F;
		float pitch = 2.0F + rand_gen_flt(server_local_sound_rng()) * 0.4F;
		server_local_emit_sound(SOUND_RANDOM_FIZZ,
								(vec3) {e->pos[0], e->pos[1], e->pos[2]}, 0.4F,
								pitch);
	}

	if(entity_item_push_out_of_blocks(s, e)) // [:53]
		correct_client = true;

	/* next-gaps-0924/W2 P12: Entity.moveEntity:262-268 isInWeb (entity.h). */
	const bool web_move = entity_web_consume(e);
	struct AABB bbox;
	entity_make_local_bbox(e, &bbox);
	bool collision_xz = false;
	for(int k = 0; k < 3; k++)
		entity_try_move(e, e->pos, e->vel, &bbox, (size_t[]) {1, 0, 2}[k],
						&collision_xz, &e->on_ground);
	if(web_move)
		glm_vec3_zero(e->vel);

	// Audit #24: post-move plate arrival trigger (POST-move box, matching
	// Beta's Entity.moveEntity ordering -- the scan runs on the box AFTER
	// entity_try_move, never the pre-move one). Cactus contact rides the same
	// sweep -- see the helper's citations.
	(void)entity_item_post_move_block_collisions(e, s);

	/* Entity.moveEntity's burning tail [Entity.java:480-496], after the
	 * block-collision sweep exactly as Beta orders it. isWet = inWater (set
	 * by this tick's onEntityUpdate) || rain at the position. */
	bool wet = server_entity_is_wet(s, in_water, e->pos[0], e->pos[1],
									e->pos[2]);
	if(entity_item_bounding_box_burning(s, e)) {
		entity_item_attack(e, 1); // dealFireDamage(1) [EntityItem.java:83-85]
		if(!wet) {
			++*fire;
			if(*fire == 0)
				*fire = 300;
		}
	} else if(*fire <= 0) {
		*fire = -1; // -fireResistance (Entity.java:52: fireResistance = 1)
	}
	if(wet && *fire > 0) {
		struct random_gen* sg = server_local_sound_rng();
		float q1 = rand_gen_flt(sg); // named locals: CODESTYLE 2.3 draw order
		float q2 = rand_gen_flt(sg);
		server_local_emit_sound(SOUND_RANDOM_FIZZ,
								(vec3) {e->pos[0], e->pos[1], e->pos[2]}, 0.7F,
								1.6F + (q1 - q2) * 0.4F); // [Entity.java:494]
		*fire = -1;
	}

	float f = e->on_ground ? entity_item_ground_friction(e) : 0.98F; // [:55-62]
	e->vel[0] *= f;			// Apply the horizontal friction factor to X [:64]
	e->vel[1] *= 0.98F;		// Apply the promoted 0.98F factor to Y [:65]
	e->vel[2] *= f;			// Apply the horizontal friction factor to Z [:66]
	/* [:67-69] Ground contact multiplies vertical motion by -0.5D, but note it
	 * multiplies ZERO in Beta: onGround is only set by a blocked downward Y
	 * move, and that same move zeroes motionY [Entity.java:417,424-426]. So a
	 * Beta item never rebounds (test_item_env_parity_golden section 3). */
	if(e->on_ground)
		e->vel[1] *= -0.5F;

	// S2: the water-entry splash edge now runs at the tick top on Beta's own
	// inWater verdict (the handleWaterMovement site above, next-gaps-0924/W2
	// P3) -- EntityItem.onUpdate:37 -> Entity.onEntityUpdate:168-190 is the
	// first thing the tick does, not a post-move probe.

	e->data.item.age++;

	// MP0 seam: the COLLECTOR is the player nearest the item (the single
	// local player today; argmin over players[] once splitscreen lands).
	struct server_player* collector = server_nearest_player(s, e->pos);

	/* A hit anywhere in this tick (cactus in the move sweep, fire/lava in
	 * onEntityUpdate or the burn tail) ran setEntityDead; the part of the
	 * tick Beta still executes has run. What a dead EntityItem can no longer
	 * do is be collected: Beta's pickup is driven from the player's own
	 * entity scan, which never visits a dead entity, and World removes it
	 * right after this onUpdate.
	 * [740c583: EntityItem.java:87-93 -> Entity.setEntityDead] */
	if(e->health <= 0 || void_dead)
		return true;
	/* Age increments last; reaching 6000 causes death. This ends
	 * EntityItem.onUpdate, and Beta's pickup is driven afterwards from the
	 * player's own scan, which skips isDead entities -- so an item that
	 * reaches 6000 this tick despawns and can no longer be collected, by the
	 * host or the guest. [EntityItem.java:71-75; EntityPlayer.java:196-203] */
	if(e->data.item.age >= ENTITY_ITEM_DESPAWN_AGE)
		return true;
	if(correct_client)
		entity_item_send_correction(s, e);

	bool pickup_phase_available = e->delay_destroy <= 0;
	if(e->delay_destroy > 0) {
		e->delay_destroy--;
	} else if(!server_transfer_actor_inert(s, collector)
			  && server_local_entity_world(s, e)
			  && server_local_entity_world(s, e) == server_local_world_of(s, collector)
			  && collector->health > 0 // [EntityPlayer.java:196] Only players with positive health scan for pickups.
			  /* the scan gate; the guest arm checks the same (itement-fix-0925) */
			  && e->data.item.pickup_delay == 0
			  && entity_item_player_overlap(e, collector)) { // FH-12: real
		// per-instance delay gate; Beta's expand(1,0,1) box (itement-fix-0925)
		// (was a flat `age >= 2*4` universal gate -- see entity.h's
		// pickup_delay field doc). [740c583: EntityItem.onCollideWithPlayer:
		// pickup requires the item delay to have reached zero.]
		// Beta semantics: the pickup consumes only what the inventory ACCEPTS.
		// on_collect drains item.count as it fills slots and returns true only
		// when the stack fully fit; on a full inventory it returns false with
		// the (possibly partially reduced) remainder still in the entity. The
		// old code IGNORED the result and destroyed the entity unconditionally
		// -- silent item LOSS whenever the inventory was full (M2 fix, pinned
		// by test_item_pickup). A failed/partial pickup just retries next tick
		// while the player stays in range (cheap: no candidate -> early false).
		bool collected = false;
		uint8_t count_before = e->data.item.item.count;
		if(collector->active_inventory && collector->active_inventory->logic
		   && collector->active_inventory->logic->on_collect)
			collected = collector->active_inventory->logic->on_collect(
				collector->active_inventory, collector,
				&e->data.item.item);

		if(collected) {
			item_emit_pickup_pop(e); // S2: Beta pops BEFORE onItemPickup
			server_local_send_entity(s, e, (struct client_rpc) {
				.type = CRPC_PICKUP_ITEM,
				.payload.pickup_item.entity_id = e->id,
				.payload.pickup_item.collector_id = 0, // local player
			});

			e->delay_destroy = 1;
		} else if(e->data.item.item.count != count_before) {
			// PARTIAL collect (review fix 2026-06-11): the inventory absorbed
			// part of the stack (a nearly-full matching slot) and the entity
			// survives with the remainder -- but only the changed inventory
			// slot was synced, never the ground entity's count, so the client
			// kept rendering the original sprite-stack size. Mirror the new
			// count. NO pop (sound or animation): Beta's
			// addItemStackToInventory compares against the stack size at the
			// start of its LAST storePartialItemStack pass, so a fill that
			// leaves a remainder returns false and onCollideWithPlayer skips
			// both the random.pop and onItemPickup (itement-fix-0925; the old
			// S2 note claiming a pop on any accepted amount misread the loop).
			// [InventoryPlayer.java:143-149; EntityItem.java:113,122]
			server_local_send_entity(s, e, (struct client_rpc) {
				.type = CRPC_ITEM_COUNT,
				.payload.item_count.entity_id = e->id,
				.payload.item_count.count = e->data.item.item.count,
			});
		}
	}

	/* Host remains first. Only a surviving stack/remainder reaches the current
	 * admitted guest, and the tick's delay/physics work is never repeated. */
	if(pickup_phase_available && e->delay_destroy <= 0
	   && e->data.item.pickup_delay == 0)
		entity_item_try_guest_collect(e, s);

	return false; // the age >= 6000 despawn returned above, before pickup
}

static void entity_render(struct entity* e, mat4 view, float tick_delta) {
	struct item* it = item_get(&e->data.item.item);

	if(it) {
		vec3 pos_lerp;
		if(e->data.item.pickup_flight_active) {
			// MP visual-parity pass item 4: flight toward the LOCAL
			// collector's render position [740c583 EntityPickupFX.
			// renderParticle] -- yOffs -0.5F is Beta's own literal argument
			// (the pickup effect constructor receives -0.5F,
			// [740c583 NetClientHandler.handleCollect]) applied to the
			// collector's eye-height Y (this engine's gstate.players[0].camera.y is the
			// same Beta posY eye reference the rest of this codebase already
			// treats it as -- see server_remote_protocol.c's own "CaveWii's
			// player Y is Beta posY" note).
			vec3 collector_pos
				= {gstate.players[0].camera.x, gstate.players[0].camera.y - 0.5F, gstate.players[0].camera.z};
			entity_pickup_flight_lerp(e->data.item.pickup_flight_start,
									  collector_pos,
									  e->data.item.pickup_flight_age,
									  tick_delta,
									  ENTITY_ITEM_PICKUP_FLIGHT_TICKS,
									  pos_lerp);
		} else {
			glm_vec3_lerp(e->pos_old, e->pos, tick_delta, pos_lerp);
		}

		struct block_data in_block;
		entity_get_block(e, floorf(pos_lerp[0]), floorf(pos_lerp[1]),
						 floorf(pos_lerp[2]), &in_block);
		render_item_update_light((in_block.torch_light << 4)
								 | in_block.sky_light);

		float ticks = e->data.item.age + tick_delta;

		mat4 model;
			glm_translate_make(model, pos_lerp);
		#ifdef GFX_3D_ELEMENTS

			glm_translate_y(model, sinf(ticks / 30.0F * GLM_PIf) * 0.1F + 0.1F);
			glm_rotate_y(model, glm_rad(ticks * 3.0F), model);
			glm_scale_uni(model, 0.25F);
			glm_translate(model, (vec3) {-0.5F, -0.5F, -0.5F});
		#endif

		mat4 mv;
		glm_mat4_mul(view, model, mv);

		#ifdef GFX_3D_ELEMENTS
			int amount = 1;
			if(e->data.item.item.count > 20) {
				amount = 4;
			} else if(e->data.item.item.count > 5) {
				amount = 3;
			} else if(e->data.item.item.count > 1) {
				amount = 2;
			}
			
					vec3 displacement[4] = {
				{0.0F, 0.0F, 0.0F},
				{-0.701F, -0.331F, -0.239F},
				{0.139F, -0.276F, 0.211F},
				{0.443F, 0.512F, -0.101F},
			};

			for(int k = 0; k < amount; k++) {
				mat4 final;
				glm_translate_make(final, displacement[k]);
				glm_mat4_mul(mv, final, final);

				it->renderItem(it, &e->data.item.item, final, false,
							   R_ITEM_ENV_ENTITY);
			}
		# else
			it->renderItem(it, &e->data.item.item, mv, false,
						 R_ITEM_ENV_ENTITY);
		#endif
		
		// EntityPickupFX draws no shadow [740c583: renderParticle never calls
		// a shadow pass -- Render.doRender's generic shadow logic only runs
		// for entities in the normal world entity-render loop, which
		// EntityPickupFX (an EntityFX/particle) never goes through].
		if(!e->data.item.pickup_flight_active) {
			struct AABB bbox;
			aabb_setsize_centered(&bbox, 0.25F, 0.25F, 0.25F);
			aabb_translate(&bbox, pos_lerp[0], pos_lerp[1] - 0.04F, pos_lerp[2]);
			entity_shadow(e, &bbox, view);
		}
	}
}

void entity_item(uint32_t id, struct entity* e, bool server, void* world,
				 struct item_data it) {
	assert(e && world);

    e->name = "Item";
	e->id = id;
//	e->drop_item = NULL;
	e->tick_server = entity_server_tick;
	e->tick_client = entity_client_tick;
	e->render = entity_render;
	e->teleport = entity_default_teleport;
	e->type = ENTITY_ITEM;
	e->data.item.age = 0;
	/* Beta Entity constructor defaults: fire=0, air=300 [Entity.java:53].
	 * fire is simulated (entity_server_tick); air stays archive-only. */
	e->data.item.persisted_fire = 0;
	e->data.item.persisted_air = 300;
	// FH-12: Java field default (Beta never initializes delayBeforeCanPickup
	// in either EntityItem constructor). Callers that need a nonzero delay
	// (10: Block.dropBlockAsItem_do / Entity.entityDropItem; 40:
	// EntityPlayer.dropPlayerItemWithRandomChoice) set it explicitly AFTER
	// this call -- see entity.h's pickup_delay field doc for the full site
	// classification.
	e->data.item.pickup_delay = 0;
	e->data.item.item = it;
	/* Both EntityItem constructors start with five health points [740c583:
	 * EntityItem.java:22,43]. Nothing in this port read an item's health
	 * before the cactus contact sweep landed, so the shared struct field was
	 * left at whatever s_clin_alloc's non-zeroing malloc returned -- an
	 * uninitialised read the moment a cactus touched it. Beta persists this
	 * value (writeEntityToNBT's "Health" tag), which the common archive
	 * Health tag already carries -- see server_entity_archive_copy. */
	e->health = 5;
	e->data.item.death_transaction = 0;
	e->data.item.death_sequence = 0;
	e->data.item.was_in_water = true; // isFirstUpdate guard: a stack spawned
	                                  // INTO water (underwater mining, death
	                                  // drops) never splashes
	// MP visual-parity pass item 4: s_clin_alloc defaults to plain malloc
	// (client_interface.c), NOT a zeroing allocator, so these need an
	// explicit false/0 start like every other field above.
	e->data.item.pickup_flight_active = false;
	e->data.item.pickup_flight_age = 0;
	glm_vec3_zero(e->data.item.pickup_flight_start);
	e->getBoundingBox = getBoundingBox;
    e->leftClickText = NULL;
    e->onLeftClick = NULL;
    e->rightClickText = NULL;
    e->onRightClick = NULL;

	entity_default_init(e, server, world);
	if(server) {
		/* Fresh lifetime Random after the ID is assigned (no world-stream
		 * draw); clients keep the zeroed state from entity_default_init. */
		const struct server_world* w = e->world;
		entity_lifetime_rng_seed(&e->gameplay_rng, w->seed, w->dimension, id);
	}
	/* EntityItem: setSize(.25,.25), yOffset=height/2. */
	entity_set_size(e, 0.25F, 0.25F, 0.125F);
}
