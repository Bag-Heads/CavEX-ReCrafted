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
#include "../block/blocks_data.h"
#include "../graphics/render_item.h"
#include "../item/items.h"
#include "../network/client_interface.h"
#include "../network/server_interface.h"
#include "../network/server_local.h"
#include "../network/server_local_state.h" // full server state (split from server_local.h)
#include "entity.h"
#include "entity_boat.h"

// Server tracker cadence: the boat's onUpdate physics are shared+deterministic
// between client and server (no RNG), so only periodic drift correction is
// needed. Mirrors entity_primed_tnt.c's own choice of 10 -- a network/
// prediction-smoothness parameter, not a simulation-correctness value; Beta's
// own EntityTrackerEntry interval was not independently re-verified for
// EntityBoat.
#define BOAT_TRACKER_INTERVAL 10

// [740c583: World.isAABBInMaterial] ported for Material.water only. Iterates
// the cell RANGE the box spans (floor(min)..floor(max+1) per axis -- max
// bounds the loop even though it is never read in the hit test itself), and
// for each WATER cell in that range computes the water column's effective
// surface height (a full cell top when metadata>=8, else (cellY+1)-meta/8 for
// the graduated flow height), returning true the MOMENT that surface reaches
// or exceeds the probe box's OWN minY. Beta's algorithm never compares
// against the probe's maxY -- a water cell entirely ABOVE the probed slice
// still counts if its surface is >= the slice's minY. Transcribed verbatim;
// this is NOT a normal AABB intersection test, do not "simplify" it into one.
static bool boat_aabb_in_water(struct entity* e, float minX, float minY,
							   float maxX, float maxY, float minZ,
							   float maxZ) {
	int x0 = (int)floorf(minX), x1 = (int)floorf(maxX + 1.0f);
	int y0 = (int)floorf(minY), y1 = (int)floorf(maxY + 1.0f);
	int z0 = (int)floorf(minZ), z1 = (int)floorf(maxZ + 1.0f);
	for(int x = x0; x < x1; x++) {
		for(int y = y0; y < y1; y++) {
			for(int z = z0; z < z1; z++) {
				struct block_data blk;
				// Unloaded cell: fail-open (not water), matching the
				// LOS predicate's unloaded-cell contract (los.h).
				if(!entity_get_block(e, x, y, z, &blk))
					continue;
				if(blk.type != BLOCK_WATER_STILL
				   && blk.type != BLOCK_WATER_FLOW)
					continue;
				float surface = (float)(y + 1);
				if(blk.metadata < 8)
					surface = (float)(y + 1) - (float)blk.metadata / 8.0f;
				if(surface >= minY)
					return true;
			}
		}
	}
	return false;
}

void boat_rider_input_velocity(float forward, float strafe, float yaw,
							   float* out_vel_x, float* out_vel_z) {
	assert(out_vel_x && out_vel_z);
	float dist = forward * forward + strafe * strafe;
	if(dist <= 0.0f) {
		*out_vel_x = 0.0f;
		*out_vel_z = 0.0f;
		return;
	}
	float distf = fmaxf(sqrtf(dist), 1.0f);
	float dx = (forward * sinf(yaw) - strafe * cosf(yaw)) / distf;
	float dz = (strafe * sinf(yaw) + forward * cosf(yaw)) / distf;
	*out_vel_x = BOAT_RIDER_INPUT_SPEED * dx;
	*out_vel_z = BOAT_RIDER_INPUT_SPEED * dz;
}

float boat_submerged_fraction(struct entity* e) {
	assert(e);
	struct AABB box;
	entity_make_world_bbox(e, &box);
	float fraction = 0.0f;
	// [740c583: EntityBoat.onUpdate] Divide the current box into five equal
	// vertical slices, each shifted down by 0.125 block. Each water-filled
	// slice adds one fifth to the submerged fraction.
	for(int i = 0; i < BOAT_BUOYANCY_SLICES; i++) {
		float slice_min = box.y1
			+ (box.y2 - box.y1) * (float)(i + 0)
				  / (float)BOAT_BUOYANCY_SLICES
			- BOAT_BUOYANCY_SLICE_Y_OFFSET;
		float slice_max = box.y1
			+ (box.y2 - box.y1) * (float)(i + 1)
				  / (float)BOAT_BUOYANCY_SLICES
			- BOAT_BUOYANCY_SLICE_Y_OFFSET;
		if(boat_aabb_in_water(e, box.x1, slice_min, box.x2, slice_max, box.z1,
							  box.z2))
			fraction += 1.0f / (float)BOAT_BUOYANCY_SLICES;
	}
	return fraction;
}

void boat_place_position(w_coord_t hit_x, w_coord_t hit_y, w_coord_t hit_z,
						 bool hit_is_snow_layer, vec3 out) {
	assert(out);
	// [740c583: ItemBoat.onItemRightClick] Lower the placement Y by one
	// when the hit block is Block.snow, the id-78 THIN LAYER
	// (BLOCK_SNOW), NOT the solid snow block (Block.blockSnow, id 80).
	w_coord_t y = hit_is_snow_layer ? hit_y - 1 : hit_y;
	out[0] = (float)hit_x + 0.5f;
	out[1] = (float)y + 1.0f;
	out[2] = (float)hit_z + 0.5f;
}

bool boat_advance(struct entity* e) {
	assert(e);
	glm_vec3_copy(e->pos, e->pos_old);

	// Hit shake/damage decay, 1/tick.
	// [740c583: EntityBoat.onUpdate] Positive hit-shake and damage counters
	// each decrease by one before buoyancy is sampled.
	if(e->data.boat.shake_ticks > 0)
		e->data.boat.shake_ticks--;
	if(e->data.boat.damage_taken > 0)
		e->data.boat.damage_taken--;

	// Buoyancy sample uses the boat's CURRENT (pre-move) box.
	float submerged = boat_submerged_fraction(e);
	if(submerged < 1.0f) {
		e->vel[1] += BOAT_BUOYANCY_PARTIAL_IMPULSE * (submerged * 2.0f - 1.0f);
	} else {
		if(e->vel[1] < 0.0f)
			e->vel[1] /= 2.0f;
		e->vel[1] += BOAT_BUOYANCY_SURFACED_IMPULSE;
	}

	// Rider steering coupling [740c583: EntityBoat.onUpdate]: an occupied
	// boat gains 0.2 times its rider's horizontal motion on each axis.
	// The source here is e->data.boat.
	// rider_vel_x/z, populated by the CALLER every tick before boat_advance
	// runs -- the client's entity_local_player.c BOAT MOUNT branch (direct
	// write, local prediction) or the server's boat_tick_server (from the
	// client-reported SRPC_PLAYER_POS vel_x/z via struct server_player.
	// rider_vel_x/z). An unoccupied boat never has this field written
	// (stays 0.0 from entity_boat()'s init), so this branch is a no-op for
	// an empty boat -- byte-identical to the pre-fix hardcoded-zero
	// behaviour. See docs/deviations.md "Boat rider steering" (closed).
	if(e->data.boat.occupied) {
		e->vel[0] += e->data.boat.rider_vel_x * BOAT_RIDER_MOTION_COUPLING;
		e->vel[2] += e->data.boat.rider_vel_z * BOAT_RIDER_MOTION_COUPLING;
	}

	if(e->vel[0] < -BOAT_MAX_HORIZONTAL_SPEED)
		e->vel[0] = -BOAT_MAX_HORIZONTAL_SPEED;
	if(e->vel[0] > BOAT_MAX_HORIZONTAL_SPEED)
		e->vel[0] = BOAT_MAX_HORIZONTAL_SPEED;
	if(e->vel[2] < -BOAT_MAX_HORIZONTAL_SPEED)
		e->vel[2] = -BOAT_MAX_HORIZONTAL_SPEED;
	if(e->vel[2] > BOAT_MAX_HORIZONTAL_SPEED)
		e->vel[2] = BOAT_MAX_HORIZONTAL_SPEED;

	// Beta reads `onGround` (this tick's not-yet-updated value, i.e. LAST
	// tick's result) here, BEFORE moveEntity recomputes it below.
	if(e->on_ground) {
		e->vel[0] *= BOAT_GROUND_DRAG;
		e->vel[1] *= BOAT_GROUND_DRAG;
		e->vel[2] *= BOAT_GROUND_DRAG;
	}

	// moveEntity(motionX, motionY, motionZ) -- Beta's exact Y, X, Z axis
	// order [740c583: Entity.moveEntity]. Beta's own field-mutation
	// semantics (a blocked axis zeroes the INSTANCE motion field) are
	// exactly entity_try_move's per-axis vel-zero-on-collision contract.
	struct AABB local_bbox;
	entity_make_local_bbox(e, &local_bbox);
	bool collided_horizontally = false;
	entity_try_move(e, e->pos, e->vel, &local_bbox, 1, &collided_horizontally,
					&e->on_ground);
	entity_try_move(e, e->pos, e->vel, &local_bbox, 0, &collided_horizontally,
					&e->on_ground);
	entity_try_move(e, e->pos, e->vel, &local_bbox, 2, &collided_horizontally,
					&e->on_ground);

	// var6 = sqrt(motionX^2+motionZ^2), read AFTER moveEntity -- i.e. the
	// POST-collision (already axis-zeroed-if-blocked) speed.
	float speed = sqrtf(e->vel[0] * e->vel[0] + e->vel[2] * e->vel[2]);

	// DEFERRED (adversarial-review finding #9, not in BOAT_DISPATCH.md's
	// numbered scope): Beta spawns "splash" particles here whenever
	// speed>0.15, independent of collided_horizontally -- 1+floor(speed*60)
	// draws of Entity.rand (a per-instance, UNSEEDED java.util.Random, per
	// the decompile; unrelated to the world/worldgen RNG stream, so skipping
	// it is not a determinism risk). Purely cosmetic client-side VFX, no
	// gameplay/test coverage in the dispatch. See BOAT_SLICE_NOTES.md.
	// [740c583: EntityBoat.onUpdate] Splash particles appear above a
	// horizontal speed of 0.15 blocks per tick.
	if(collided_horizontally && speed > BOAT_LAND_CRASH_SPEED_THRESHOLD) {
		// LAND CRASH [740c583: EntityBoat.onUpdate]: horizontal collision at
		// speed above 0.15 kills the boat, dropping three planks and two
		// sticks. Beta does NOT return here -- the
		// yaw-turn/entity-push/snow-melt tail still runs on the
		// now-isDead=true entity that same tick. That tail is unobservable
		// once the entity is removed (which the caller does), so skipping
		// it is behaviorally equivalent, not a deviation. The caller
		// performs the drops/dismount/removal side effects.
		return true;
	}

	e->vel[0] *= BOAT_DRAG_XZ;
	e->vel[1] *= BOAT_DRAG_Y; // Y differs from X/Z on purpose
	e->vel[2] *= BOAT_DRAG_XZ;

	// Turn-toward-motion yaw [740c583: EntityBoat.onUpdate]. Beta computes
	// this in DEGREES from the BACKWARD delta (prevPos - pos); CaveWii
	// stores yaw in radians (orient[0]), so the wrap/clamp constants are
	// pre-converted (BOAT_YAW_TURN_MAX_RAD) rather than re-deriving degrees.
	float dx = e->pos_old[0] - e->pos[0];
	float dz = e->pos_old[2] - e->pos[2];
	if(dx * dx + dz * dz > 0.001f) {
		float target = atan2f(dz, dx);
		float delta = target - e->orient[0];
		while(delta >= GLM_PIf)
			delta -= 2.0f * GLM_PIf;
		while(delta < -GLM_PIf)
			delta += 2.0f * GLM_PIf;
		if(delta > BOAT_YAW_TURN_MAX_RAD)
			delta = BOAT_YAW_TURN_MAX_RAD;
		if(delta < -BOAT_YAW_TURN_MAX_RAD)
			delta = -BOAT_YAW_TURN_MAX_RAD;
		e->orient[0] += delta;
	}

	// DEFERRED (not in BOAT_DISPATCH.md's numbered scope): the entity-push
	// exchange with OTHER boats (applyEntityCollision) and the 4-corner
	// under-hull snow-melt check. See BOAT_SLICE_NOTES.md.
	return false;
}

static size_t boat_get_bounding_box(const struct entity* e, struct AABB* out) {
	assert(e && out);
	entity_make_world_bbox(e, out);
	return 1;
}

static bool boat_tick_client(struct entity* e) {
	assert(e);
	// The server owns removal; never self-free a network entity (mirrors
	// entity_primed_tnt.c's awaiting_destroy precedent).
	if(e->data.boat.awaiting_destroy)
		return false;
	if(boat_advance(e))
		e->data.boat.awaiting_destroy = true;
	return false;
}

void boat_spawn_destruction_drops_w(vec3 pos, struct server_world* w) {
	assert(pos);
	// [740c583: EntityBoat.attackEntityFrom / EntityBoat.onUpdate]
	// Drop three separate plank items followed by two separate sticks.
	// Each drop uses zero vertical offset.
	// dropItemWithOffset(id,count,0.0F) spawns AT the entity's exact position
	// (offset only ever touches Y, and it's 0 here) with the ordinary
	// EntityItem constructor hover velocity -- `throw=false`, matching
	// entity_minecart.c's own dropItemWithOffset(minecartEmpty) call. FIVE
	// separate 1-count spawns (not one stacked spawn of 3/2) so the RNG draw
	// COUNT stays Beta-faithful (each spawn independently consumes the
	// constructor's hover/velocity draws).
	// FH-12: dropItemWithOffset -> entityDropItem -> delayBeforeCanPickup = 10,
	// for both the planks and the stick drops. [740c583: Entity.entityDropItem]
	struct item_data planks = {.id = BLOCK_PLANKS, .count = 1, .durability = 0};
	for(int i = 0; i < 3; i++) {
		struct entity* plank_drop = server_local_spawn_item_w(w, pos, &planks, false);
		if(plank_drop)
			plank_drop->data.item.pickup_delay = 10;
	}
	struct item_data stick = {.id = ITEM_STICK, .count = 1, .durability = 0};
	for(int i = 0; i < 2; i++) {
		struct entity* stick_drop = server_local_spawn_item_w(w, pos, &stick, false);
		if(stick_drop)
			stick_drop->data.item.pickup_delay = 10;
	}
}

void boat_spawn_destruction_drops(vec3 pos, struct server_local* s) {
	/* Legacy callback bridge: Entity.entityDropItem uses worldObj. Outside
	 * a world dispatch block_world still resolves to slot 0. */
	boat_spawn_destruction_drops_w(pos, block_world(s));
}

bool boat_send_correction(struct entity* e) {
	assert(e && e->on_server && e->type == ENTITY_BOAT);
	struct client_rpc call = {
		.type = CRPC_BOAT_CORRECTION,
		.payload.boat_state = {
			.entity_id = e->id,
			.pos = {e->pos[0], e->pos[1], e->pos[2]},
			.vel = {e->vel[0], e->vel[1], e->vel[2]},
			.yaw = e->orient[0],
			.damage_taken = e->data.boat.damage_taken,
			.shake_ticks = e->data.boat.shake_ticks,
			.shake_dir = e->data.boat.shake_dir,
			.occupied = e->data.boat.occupied,
			.occupant_id = e->data.boat.occupant_id,
			.on_ground = e->on_ground,
		},
	};
	/* G8-VEHICLE-SEAMS-01: stamped with the boat's own world, so a correction
	 * never reaches another live world's payload under a reused id. */
	if(!server_local_try_send_entity(server_local_of_entity(e), e, call))
		return false;
	e->data.boat.correction_ticks = 0;
	return true;
}

static bool boat_tick_server(struct entity* e, struct server_local* s) {
	assert(e && s && e->on_server && e->world == (void*)server_local_entity_world(s, e));

	/* Explosion entity pass latch (server_world.c) -- the blast's
	 * attackEntityFrom `> 40` break, same drop shape as the land crash below
	 * and the melee break in server_local.c; deferred to the boat's own tick
	 * because the pass runs inside the entity dict iteration.
	 * [740c583 EntityBoat.attackEntityFrom:63-85] */
	if(e->data.boat.blast_break_pending
	   || e->data.boat.damage_taken > BOAT_DESTROY_DAMAGE_THRESHOLD) {
		if(e->data.boat.occupied)
			server_local_set_boat_occupancy(s, e, false);
		boat_spawn_destruction_drops_w(e->pos, server_local_entity_world(s, e));
		return true;
	}

	// Rider steering SOURCE (server side): copy the client-reported input
	// velocity onto the boat right before boat_advance's occupied-branch
	// coupling reads it. CaveWii models exactly one possible occupant (the
	// local player -- struct entity_boat.occupant_id's own comment), so
	// s->player IS the (only) possible rider whenever occupied.
	if(e->data.boat.occupied) {
		/* S1-a2: resolve the RIDER identity rather than assuming the host.
		 * Beta reads riddenByEntity.motionX/Z [740c583 EntityBoat.onUpdate];
		 * with a local split guest, assuming s->player fed the HOST input into
		 * a boat the guest was riding. A rider whose session ended (leave,
		 * release, rejoin) is force-DISMOUNTED here -- Beta drops
		 * riddenByEntity for a dead/absent rider [740c583 Entity.updateRidden]
		 * -- which also stops the boat from steering itself from stale input.
		 * Host rider: the zero identity resolves to &s->player, so both lines
		 * are the ones they replaced. */
		/* G8-VEHICLE-SEAMS-01: the relation must also name the rider wire id
		 * and the boat world; a wrong-world or reused-id relation is dropped
		 * like a departed rider. */
		struct server_player* rider = server_local_vehicle_rider(s, e,
			e->data.boat.rider_actor, e->data.boat.occupant_id);
		if(!rider) {
			server_local_set_boat_occupancy(s, e, false);
		} else {
		e->data.boat.rider_vel_x = rider->rider_vel_x;
		e->data.boat.rider_vel_z = rider->rider_vel_z;
		}
	}

	if(boat_advance(e)) {
		// LAND CRASH destroy side effects
		// [740c583: EntityBoat.onUpdate destroy branch]. Same drop shape as
		// attackEntityFrom's >40 branch -- see server_local.c's
		// SRPC_ENTITY_ATTACK ENTITY_BOAT case for the shared drop helper.
		if(e->data.boat.occupied)
			server_local_set_boat_occupancy(s, e, false);
		boat_spawn_destruction_drops_w(e->pos, server_local_entity_world(s, e));
		return true; // generic tick loop frees + sends CRPC_ENTITY_DESTROY
	}

	if(e->data.boat.correction_ticks < BOAT_TRACKER_INTERVAL)
		e->data.boat.correction_ticks++;
	if(e->data.boat.correction_ticks >= BOAT_TRACKER_INTERVAL)
		(void)boat_send_correction(e);
	return false;
}

// CLIENT PREDICTION: rock the boat the instant the punch lands (mirrors
// entity_minecart.c's onLeftClick exactly). This is a local fist-strength
// GUESS (+=10, matching the correct attackEntityFrom shape
// the damage amount contributes ten times its value), rather than Beta's
// performHurtAnimation self-multiplying damage bug --
// see BOAT_SLICE_NOTES.md for why that Notch bug is deliberately not
// replicated here. The AUTHORITATIVE damage/break lives in the server's
// SRPC_ENTITY_ATTACK handler.
static bool onLeftClick(struct entity* e) {
	assert(e);
	e->data.boat.shake_dir = (int8_t)-e->data.boat.shake_dir;
	e->data.boat.shake_ticks = 10;
	e->data.boat.damage_taken = (int16_t)(e->data.boat.damage_taken + 10);

	svin_rpc_send(&(struct server_rpc){
		.type = SRPC_ENTITY_ATTACK,
		.payload.entity_attack.entity_id = e->id,
	});

	return true;
}

static void entity_boat_render(struct entity* e, mat4 view, float tick_delta) {
	assert(e);
	vec3 pos_lerp;
	glm_vec3_lerp(e->pos_old, e->pos, tick_delta, pos_lerp);

	struct block_data in_block = {0};
	(void)entity_get_block(e, (w_coord_t)floorf(pos_lerp[0]),
						   (w_coord_t)floorf(pos_lerp[1]),
						   (w_coord_t)floorf(pos_lerp[2]), &in_block);
	render_item_update_light((in_block.torch_light << 4) | in_block.sky_light);

	// No dedicated 3D boat mesh/texture exists in this tree yet -- Beta's own
	// asset is a SEPARATE "/item/boat.png" model texture, distinct from the
	// items.png inventory-icon atlas [740c583: RenderBoat.func_157_a]. Render
	// the boat's own flat item icon as a placeholder world billboard,
	// matching the entity_primed_tnt.c precedent (an entity without a custom
	// mesh reuses its item icon). A modeled boat mesh is future
	// rendering-polish work, not a simulation gap; see BOAT_SLICE_NOTES.md.
	struct item_data stack = {.id = ITEM_BOAT, .count = 1, .durability = 0};
	struct item* item = item_get(&stack);
	if(!item || !item->renderItem)
		return;

	mat4 model, mv;
	glm_translate_make(model, pos_lerp);
	// RenderBoat.func_157_a rotates around Y by 180 degrees minus yaw_deg.
	// [740c583: RenderBoat]. CaveWii stores yaw in radians (orient[0]); the
	// mesh-facing convention (180 degrees = PI radians) carries over exactly.
	glm_rotate_y(model, GLM_PIf - e->orient[0], model);

	// Hit shake -- IDENTICAL formula to RenderMinecart's (entity_minecart.c
	// precedent), confirmed by decompile: both vehicle classes share this
	// rock-about-local-X shape byte for byte.
	// [740c583: RenderBoat.func_157_a]
	{
		float shake_f = (float)e->data.boat.shake_ticks - tick_delta;
		float dmg_f = (float)e->data.boat.damage_taken - tick_delta;
		if(dmg_f < 0.0f)
			dmg_f = 0.0f;
		if(shake_f > 0.0f)
			glm_rotate_x(model,
						 glm_rad(sinf(shake_f) * shake_f * dmg_f / 10.0f
								 * (float)e->data.boat.shake_dir),
						 model);
	}

	glm_scale_uni(model, BOAT_WIDTH);
	glm_translate(model, (vec3){-0.5f, -0.15f, -0.5f});
	glm_mat4_mul(view, model, mv);
	item->renderItem(item, &stack, mv, false, R_ITEM_ENV_ENTITY);

	struct AABB bbox;
	aabb_setsize_centered(&bbox, BOAT_WIDTH * 0.5f, 0.25f, BOAT_WIDTH * 0.5f);
	aabb_translate(&bbox, pos_lerp[0], pos_lerp[1] - 0.04f, pos_lerp[2]);
	entity_shadow(e, &bbox, view);
}

void entity_boat(uint32_t id, struct entity* e, bool server, void* world) {
	assert(e && world);
	entity_default_init(e, server, world);
	// Beta EntityBoat ctor: width 1.5F, height 0.6F, yOffset half the height.
	// [740c583: EntityBoat.<init>(World)]
	entity_set_size(e, BOAT_WIDTH, BOAT_HEIGHT, BOAT_HEIGHT * 0.5f);
	e->id = id;
	e->type = ENTITY_BOAT;
	e->name = "Boat";
	e->tick_server = boat_tick_server;
	e->tick_client = boat_tick_client;
	e->render = entity_boat_render;
	e->teleport = entity_default_teleport;
	e->getBoundingBox = boat_get_bounding_box;
	e->leftClickText = NULL;
	e->onLeftClick = onLeftClick;
	e->rightClickText = "Ride";
	e->onRightClick = NULL;
	e->data.boat.damage_taken = 0;
	e->data.boat.blast_break_pending = false;
	e->data.boat.shake_ticks = 0;
	e->data.boat.shake_dir = 1; // Beta boatRockDirection starts at 1, not 0
	e->data.boat.occupied = false;
	e->data.boat.occupant_id = 0;
	e->data.boat.rider_actor = ACTOR_IDENT_HOST; // S1-a2: never pre-ridden
	e->data.boat.correction_ticks = 0;
	e->data.boat.awaiting_destroy = false;
	e->data.boat.been_attacked = false; // [740c583: Entity.<init>] Start with the attacked flag clear.
	e->data.boat.rider_vel_x = 0.0f;
	e->data.boat.rider_vel_z = 0.0f;
}
