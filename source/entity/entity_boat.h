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

#ifndef ENTITY_BOAT_H
#define ENTITY_BOAT_H

#include <stdbool.h>
#include <stdint.h>

#include "../block/blocks_data.h" // w_coord_t
#include "../cglm/cglm.h"

struct entity;
struct server_local;

/* Beta 1.7.3 EntityBoat constants [740c583: EntityBoat, ItemBoat]. */
#define BOAT_WIDTH 1.5f
#define BOAT_HEIGHT 0.6f
/* boatCurrentDamage accumulates amount*10 per hit; STRICT > 40 destroys
 * (40 survives). [740c583: EntityBoat.attackEntityFrom] */
#define BOAT_HIT_DAMAGE_SCALE 10
#define BOAT_DESTROY_DAMAGE_THRESHOLD 40
/* Land-crash gate: isCollidedHorizontally && sqrt(vx^2+vz^2) > 0.15
 * [740c583: EntityBoat.onUpdate]. */
#define BOAT_LAND_CRASH_SPEED_THRESHOLD 0.15f
/* Post-move drag, applied only when NOT destroyed this tick. Y differs from
 * X/Z on purpose -- do not "simplify" to one constant. */
#define BOAT_DRAG_XZ 0.99f
#define BOAT_DRAG_Y 0.95f
/* Buoyancy: 5 Y-slices of the boat's own bounding box, each offset -0.125
 * from its nominal top/bottom, tested against Material.water via
 * World.isAABBInMaterial. [740c583: EntityBoat.onUpdate] */
#define BOAT_BUOYANCY_SLICES 5
#define BOAT_BUOYANCY_SLICE_Y_OFFSET 0.125f
#define BOAT_BUOYANCY_PARTIAL_IMPULSE 0.04f
#define BOAT_BUOYANCY_SURFACED_IMPULSE 0.007f
/* Rider steering coupling: motionX/Z += riddenByEntity.motionX/Z * 0.2.
 * [740c583: EntityBoat.onUpdate]. The rider-motion SOURCE (docs/
 * deviations.md "Boat rider steering") is struct entity_boat's own
 * rider_vel_x/z, populated per-tick by the caller -- see boat_advance's
 * occupied branch and boat_rider_input_velocity below. */
#define BOAT_RIDER_MOTION_COUPLING 0.2f
/* Rider input->motion speed while boat-mounted, feeding
 * boat_rider_input_velocity below. Beta's rider keeps running
 * Entity.moveFlying(strafe, forward, speed) every tick via
 * Entity.updateRidden (zero then re-derive) while mounted [740c583:
 * Entity.updateRidden, EntityLiving.moveEntityWithHeading]. A boat-mounted
 * rider's own onGround is never meaningfully tracked here (position is
 * fully tethered to the boat, never run through moveEntity), matching
 * Beta's real seated/floating case -- moveEntityWithHeading's in-water,
 * in-lava, AND not-onGround branches ALL pass moveFlying this exact flat
 * speed; only the onGround branch differs (a per-block-slipperiness
 * formula, irrelevant to a floating rider). [740c583:
 * EntityLiving.moveEntityWithHeading, the flat 0.02F moveFlying speed] */
#define BOAT_RIDER_INPUT_SPEED 0.02f
#define BOAT_MAX_HORIZONTAL_SPEED 0.4f
#define BOAT_GROUND_DRAG 0.5f
/* Turn-toward-motion yaw clamp, Beta degrees converted once to radians
 * (20.0D -> 0.3490658503988659 rad). [740c583: EntityBoat.onUpdate] */
#define BOAT_YAW_TURN_MAX_RAD 0.3490658503988659f
/* ItemBoat.onItemRightClick: a 5.0-block eye-ray, stopping on the first
 * non-air cell (Beta's rayTraceBlocks_do(.., useLiquids=true) -- liquids are
 * NOT skipped, unlike the ordinary block-highlight raycast). */
#define BOAT_PLACE_REACH 5.0f
/* Adversarial-review findings #2/#3: EntityBoat.getMountedYOffset() ALWAYS
 * returns height*0.0 - 0.3 (the height*0.0 term is a Notch no-op, transcribed
 * literally per the dispatch's own "Two facts that will look like bugs"
 * precedent -- it never varies with the boat's own height). [740c583:
 * EntityBoat.getMountedYOffset] */
#define BOAT_MOUNTED_Y_OFFSET -0.3f
/* EntityBoat's OWN override of updateRiderPosition (Entity's generic version
 * has no horizontal term at all -- confirmed by decompile, both read):
 *   the rider is placed at x + cos(yaw_rad)*0.4, at y plus
 *   the mounted Y offset plus the rider's own Y offset, and at z + sin(yaw_rad)*0.4
 * Beta's rotationYaw is in DEGREES (cos/sin take the *pi/180 conversion);
 * CaveWii's e->orient[0] is ALREADY radians in the identical atan2(dz,dx)
 * convention boat_advance's own turn-toward-motion code uses, so no
 * conversion is needed at the call site; X/Z use double trig and addition
 * before the final float storage conversion.
 * [740c583: EntityBoat.updateRiderPosition] */
#define BOAT_RIDER_MOUNT_HORIZONTAL_OFFSET 0.4

void entity_boat(uint32_t id, struct entity* e, bool server, void* world);

/* Shared Beta EntityBoat.onUpdate physics (buoyancy, rider coupling, move,
 * drag, turn-toward-motion). Runs identically on client and server so client
 * prediction tracks the deterministic case; only hit damage/shake and the
 * land-crash DESTROY decision are server-authoritative side effects, applied
 * by the caller. Returns true iff THIS tick's land-crash condition fired
 * (isCollidedHorizontally && post-move speed > 0.15); the caller is
 * responsible for the drop/dismount/removal side effects Beta performs in
 * that branch instead of the drag/turn tail. */
bool boat_advance(struct entity* e);

/* Beta World.isAABBInMaterial water test on a slice of the boat's bounding box
 * summed over the BOAT_BUOYANCY_SLICES horizontal slabs -- exposed standalone
 * so the slice count/offset can be pinned independent of the rest of
 * onUpdate. Returns the submerged fraction in [0.0, 1.0]. */
float boat_submerged_fraction(struct entity* e);

/* Rider steering INPUT: Beta's Entity.moveFlying direction/speed shape
 * (rotate a forward/strafe pair by yaw, normalize so diagonal input isn't
 * faster than axis-aligned input, scale by BOAT_RIDER_INPUT_SPEED) --
 * [740c583: Entity.moveFlying] the input length is sqrt(strafe^2+forward^2); a zero
 * length adds nothing [mirrors the sibling on-foot formula's own `dist>0.0F` gate in
 * entity_local_player.c rather than Beta's 0.01F length threshold -- forward/
 * strafe are always exactly {-1,-0.3,0,0.3,1} from this engine's digital
 * input reads, so the two gates are unreachably different]; otherwise the
 * length is raised to at least 1, both inputs scale by speed over it, X motion gains
 * strafe*cos(yaw)-forward*sin(yaw) and Z motion gains forward*cos(yaw)+
 * strafe*sin(yaw). Exposed (not static) so it is independently testable
 * against the decompile without a real input backend; consumed by
 * entity_local_player.c's BOAT MOUNT branch. */
void boat_rider_input_velocity(float forward, float strafe, float yaw,
							   float* out_vel_x, float* out_vel_z);

/* ItemBoat.onItemRightClick spawn placement: (blockX+0.5, blockY+1.0,
 * blockZ+0.5), except blockY-1.0 when the hit cell is the thin snow LAYER
 * (BLOCK_SNOW, id 78 -- NOT the solid snow block). */
void boat_place_position(w_coord_t hit_x, w_coord_t hit_y, w_coord_t hit_z,
						 bool hit_is_snow_layer, vec3 out);

/* The exact drop multiset Beta emits on BOTH boat destruction paths
 * [740c583: EntityBoat.attackEntityFrom AND EntityBoat.onUpdate land-crash]:
 * 3 individual 1-count planks entities, THEN 2 individual 1-count stick
 * entities (5 separate dropItemWithOffset(.., .., 0.0F) calls, never one
 * stacked spawn) -- shared here so the two call sites (server_local.c's
 * SRPC_ENTITY_ATTACK >40 branch and this file's own land-crash tick) cannot
 * drift apart (§3.2 extract-on-second-transcription).
 */
void boat_spawn_destruction_drops(vec3 pos, struct server_local* s);
struct server_world;
void boat_spawn_destruction_drops_w(vec3 pos, struct server_world* w);

/* Force an out-of-band CRPC_BOAT_CORRECTION send (not gated on the periodic
 * tracker cadence) -- used right after an authoritative occupancy change so
 * the client learns it promptly instead of waiting up to
 * BOAT_TRACKER_INTERVAL ticks. Returns false (transport backlog) exactly
 * like the periodic send; correction_ticks is left dirty so the ordinary
 * cadence retries it. */
bool boat_send_correction(struct entity* e);

#endif
